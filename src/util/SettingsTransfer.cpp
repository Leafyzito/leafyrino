// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/SettingsTransfer.hpp"

#include "singletons/Settings.hpp"
#include "util/CombinePath.hpp"

#include <pajlada/settings/settingmanager.hpp>
#include <pajlada/settings/signalargs.hpp>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>
#include <rapidjson/document.h>

#include <algorithm>
#include <initializer_list>

using namespace Qt::Literals;

namespace {

using namespace chatterino;
using namespace chatterino::settingstransfer;

const QString FORMAT_NAME = u"leafyrino-settings"_s;
// Moltorino's exports use the same format
const QString MOLTORINO_FORMAT_NAME = u"moltorino-settings"_s;
constexpr int FORMAT_VERSION = 1;

const QString SETTINGS_FILE = u"settings.json"_s;
const QString LAYOUT_FILE = u"window-layout.json"_s;
const QString COMMANDS_FILE = u"commands.json"_s;
const QString USER_DATA_FILE = u"user-data.json"_s;

const QString STAGING_DIRECTORY = u".leafyrino-restore"_s;
const QString MANIFEST_FILE = u"pending.json"_s;
const QString BACKUP_INFIX = u".bkp-"_s;
const QString RESCUE_INFIX = u".rescue-"_s;
constexpr qsizetype MAX_RESCUE_COPIES = 9;

const QString CATEGORY_APPEARANCE = u"appearance"_s;
const QString CATEGORY_CHAT = u"chat"_s;
const QString CATEGORY_LAYOUT = u"layout"_s;
const QString CATEGORY_HIGHLIGHTS = u"highlights"_s;
const QString CATEGORY_COMMANDS = u"commands"_s;
const QString CATEGORY_LISTS = u"lists"_s;
const QString CATEGORY_MODERATION = u"moderation"_s;
const QString CATEGORY_NOTIFICATIONS = u"notifications"_s;
const QString CATEGORY_STREAMER_MODE = u"streamer-mode"_s;
const QString CATEGORY_FEATURES = u"features"_s;
const QString CATEGORY_PERSONAL = u"personal"_s;
const QString CATEGORY_ADVANCED = u"advanced"_s;
const QString CATEGORY_OTHER = u"other"_s;

/// Categories that can be applied without restarting
const QSet<QString> LIVE_CATEGORIES{CATEGORY_APPEARANCE};

/// Settings in live categories that are only read on startup (lowercase)
const QSet<QString> RESTART_SETTINGS{u"/appearance/uselegacyscaling"_s};

/// Settings files that are exported as a whole
struct TransferFile {
    QString name;
    QString category;
    /// Key of the file in an export
    QString key;
};

const std::vector<TransferFile> TRANSFER_FILES{
    {COMMANDS_FILE, CATEGORY_COMMANDS, u"commands"_s},
    {LAYOUT_FILE, CATEGORY_LAYOUT, u"layout"_s},
    {USER_DATA_FILE, CATEGORY_PERSONAL, u"userData"_s},
};

QString categoryForFile(const QString &file)
{
    for (const auto &transferFile : TRANSFER_FILES)
    {
        if (transferFile.name == file)
        {
            return transferFile.category;
        }
    }
    return {};
}

bool isRestorableFile(const QString &file)
{
    return file == SETTINGS_FILE || !categoryForFile(file).isEmpty();
}

bool isValidContent(const QString &file, const QJsonValue &value)
{
    if (file == LAYOUT_FILE)
    {
        // the layout doesn't load without a window
        return value.isObject() &&
               !value.toObject().value(u"windows"_s).toArray().isEmpty();
    }
    return value.isObject();
}

/// Returns true if `path` is `prefix` or inside of it
bool isUnder(QStringView path, QStringView prefix)
{
    return path.startsWith(prefix) &&
           (path.size() == prefix.size() || path.at(prefix.size()) == u'/');
}

bool containsAny(QStringView text, std::initializer_list<QStringView> parts)
{
    return std::ranges::any_of(parts, [&](QStringView part) {
        return text.contains(part);
    });
}

/// Returns true for keys inside of values that are never exported or imported
bool isPrivateKey(const QString &key)
{
    return containsAny(
        key.toLower(),
        {u"token", u"secret", u"password", u"credential", u"cookie", u"oauth",
         u"clientid", u"apikey", u"api_key", u"authorization", u"customsound",
         u"soundurl", u"filepath", u"directory"});
}

QJsonValue withoutPrivateKeys(const QJsonValue &value)
{
    if (value.isObject())
    {
        QJsonObject result;
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
        {
            if (!isPrivateKey(it.key()))
            {
                result.insert(it.key(), withoutPrivateKeys(it.value()));
            }
        }
        return result;
    }
    if (value.isArray())
    {
        QJsonArray result;
        const auto array = value.toArray();
        for (const auto &item : array)
        {
            result.append(withoutPrivateKeys(item));
        }
        return result;
    }
    return value;
}

ExpectedStr<QByteArray> readFile(const QString &path)
{
    const auto name = QFileInfo(path).fileName();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return makeUnexpected(u"Couldn't read %1."_s.arg(name));
    }
    auto contents = file.read(MAX_FILE_SIZE + 1);
    if (file.error() != QFileDevice::NoError)
    {
        return makeUnexpected(u"Couldn't read %1."_s.arg(name));
    }
    if (contents.size() > MAX_FILE_SIZE)
    {
        return makeUnexpected(u"%1 is too large."_s.arg(name));
    }
    return contents;
}

ExpectedStr<QJsonValue> parseJson(const QByteArray &contents)
{
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(contents, &error);
    if (error.error != QJsonParseError::NoError)
    {
        return makeUnexpected(u"invalid JSON (%1)"_s.arg(error.errorString()));
    }
    if (document.isArray())
    {
        return QJsonValue(document.array());
    }
    return QJsonValue(document.object());
}

ExpectedStr<void> writeFile(const QString &path, const QByteArray &contents)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(contents) != contents.size() || !file.commit())
    {
        return makeUnexpected(
            u"Couldn't write %1."_s.arg(QFileInfo(path).fileName()));
    }
    return {};
}

QByteArray toJson(const QJsonValue &value)
{
    if (value.isArray())
    {
        return QJsonDocument(value.toArray()).toJson(QJsonDocument::Indented);
    }
    return QJsonDocument(value.toObject()).toJson(QJsonDocument::Indented);
}

/// Reads a settings file, which doesn't have to exist
ExpectedStr<QJsonValue> readSettingsFile(const QString &settingsDirectory,
                                         const QString &file)
{
    const auto path = combinePath(settingsDirectory, file);
    if (!QFileInfo::exists(path))
    {
        return QJsonValue(QJsonObject{});
    }
    auto contents = readFile(path);
    if (!contents)
    {
        return makeUnexpected(contents.error());
    }
    auto json = parseJson(*contents);
    if (!json)
    {
        return makeUnexpected(u"%1 is damaged: %2"_s.arg(file, json.error()));
    }
    return json;
}

/// Returns what identifies a list entry, so merged lists don't get duplicates
QByteArray mergeIdentity(const QJsonValue &value)
{
    if (value.isObject())
    {
        const auto object = value.toObject();
        const auto id = object.value(u"id"_s).toVariant().toString();
        if (!id.isEmpty())
        {
            return "id:" + id.toCaseFolded().toUtf8();
        }

        // nicknames
        const auto name = object.value(u"name"_s).toVariant().toString();
        if (!name.isEmpty() && object.contains(u"replace"_s))
        {
            const bool regex = object.value(u"isRegex"_s).toBool(false);
            const bool caseSensitive =
                object.value(u"isCaseSensitive"_s).toBool(false);
            const QJsonObject identity{
                {u"name"_s,
                 regex || caseSensitive ? name : name.toCaseFolded()},
                {u"isRegex"_s, regex},
                {u"isCaseSensitive"_s, caseSensitive},
            };
            return "nickname:" +
                   QJsonDocument(identity).toJson(QJsonDocument::Compact);
        }
        if (!name.isEmpty())
        {
            return "name:" + name.toCaseFolded().toUtf8();
        }

        // highlights and ignores
        const auto pattern = object.value(u"pattern"_s).toString();
        if (!pattern.isEmpty())
        {
            const bool regex = object.value(u"regex"_s).toBool(false);
            const bool caseSensitive =
                object.contains(u"case"_s)
                    ? object.value(u"case"_s).toBool(false)
                    : object.value(u"caseSensitive"_s).toBool(false);
            const QJsonObject identity{
                {u"pattern"_s,
                 regex || caseSensitive ? pattern : pattern.toCaseFolded()},
                {u"regex"_s, regex},
                {u"case"_s, caseSensitive},
                {u"channelScope"_s, object.value(u"channelScope"_s)},
                {u"channelTargets"_s, object.value(u"channelTargets"_s)},
                {u"global"_s, object.value(u"global"_s)},
                {u"channels"_s, object.value(u"channels"_s)},
                {u"ExcludedChannels"_s, object.value(u"ExcludedChannels"_s)},
            };
            return "pattern:" +
                   QJsonDocument(identity).toJson(QJsonDocument::Compact);
        }

        for (const auto &key : {u"userID"_s, u"command"_s})
        {
            const auto identity = object.value(key).toVariant().toString();
            if (!identity.isEmpty())
            {
                return key.toUtf8() + ':' + identity.toCaseFolded().toUtf8();
            }
        }
    }
    return QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
}

QJsonValue mergedValue(const QJsonValue &current, const QJsonValue &incoming,
                       bool mergeLists)
{
    if (!mergeLists)
    {
        return incoming;
    }
    if (current.isObject() && incoming.isObject())
    {
        auto merged = current.toObject();
        const auto incomingObject = incoming.toObject();
        for (auto it = incomingObject.begin(); it != incomingObject.end(); ++it)
        {
            merged.insert(it.key(), mergedValue(merged.value(it.key()),
                                                it.value(), true));
        }
        return merged;
    }
    if (current.isArray() && incoming.isArray())
    {
        auto merged = current.toArray();
        QHash<QByteArray, qsizetype> positions;
        for (qsizetype i = 0; i < merged.size(); ++i)
        {
            positions.insert(mergeIdentity(merged.at(i)), i);
        }
        const auto incomingArray = incoming.toArray();
        for (const auto &value : incomingArray)
        {
            const auto identity = mergeIdentity(value);
            const auto existing = positions.constFind(identity);
            if (existing != positions.cend())
            {
                merged.replace(*existing, value);
            }
            else
            {
                merged.append(value);
                positions.insert(identity, merged.size() - 1);
            }
        }
        return merged;
    }
    return incoming;
}

void setAtPath(QJsonObject &root, const QStringList &segments, qsizetype index,
               const QJsonValue &value, bool mergeLists)
{
    if (index >= segments.size())
    {
        return;
    }
    const auto &key = segments.at(index);
    if (index == segments.size() - 1)
    {
        root.insert(key, mergedValue(root.value(key), value, mergeLists));
        return;
    }

    auto child = root.value(key).toObject();
    setAtPath(child, segments, index + 1, value, mergeLists);
    root.insert(key, child);
}

/// Returns the value at `path`, or an undefined value if there's none
QJsonValue valueAtPath(const QJsonObject &root, const QString &path)
{
    QJsonValue current = root;
    for (const auto &segment : path.split(u'/', Qt::SkipEmptyParts))
    {
        if (!current.isObject())
        {
            return QJsonValue(QJsonValue::Undefined);
        }
        current = current.toObject().value(segment);
    }
    return current;
}

/// Collects the values in settings.json by path. Registered settings are kept
/// whole, other objects are split up into their values.
void flattenSettings(const QJsonValue &value, const QString &path,
                     const QJsonObject &registered, QJsonObject &flat)
{
    if (!path.isEmpty() && registered.contains(path))
    {
        flat.insert(path, registered.value(path));
        return;
    }
    if (value.isObject())
    {
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
        {
            flattenSettings(it.value(), path % u'/' % it.key(), registered,
                            flat);
        }
        return;
    }
    if (!path.isEmpty())
    {
        flat.insert(path, value);
    }
}

/// Copies a settings file next to itself before it's replaced, returns the
/// path of the copy
ExpectedStr<QString> makeRescueCopy(const QString &path)
{
    // the time comes first, so the names sort by age
    const QString rescuePath =
        path % RESCUE_INFIX %
        QDateTime::currentDateTimeUtc().toString(u"yyyyMMdd-HHmmss-zzz"_s) %
        u'-' % QUuid::createUuid().toString(QUuid::Id128).left(8);
    if (!QFile::copy(path, rescuePath))
    {
        return makeUnexpected(u"Couldn't make a rescue copy of %1."_s.arg(
            QFileInfo(path).fileName()));
    }
    return rescuePath;
}

void removeOldRescueCopies(const QString &settingsDirectory,
                           const QString &file)
{
    const auto copies =
        QDir(settingsDirectory)
            .entryInfoList({file % RESCUE_INFIX % u'*'}, QDir::Files,
                           QDir::Name | QDir::Reversed);
    for (auto i = MAX_RESCUE_COPIES; i < copies.size(); ++i)
    {
        QFile::remove(copies.at(i).absoluteFilePath());
    }
}

/// Writes `files` (name -> contents) to the staging directory, replacing
/// anything that was staged before
ExpectedStr<void> stageFiles(const QString &settingsDirectory,
                             const QHash<QString, QByteArray> &files)
{
    if (files.isEmpty())
    {
        return makeUnexpected(u"There's nothing to restore."_s);
    }

    const auto staging = combinePath(settingsDirectory, STAGING_DIRECTORY);
    QDir(staging).removeRecursively();
    if (!QDir().mkpath(staging))
    {
        return makeUnexpected(u"Couldn't prepare the restore."_s);
    }

    const auto fail = [&](const QString &error) -> ExpectedStr<void> {
        QDir(staging).removeRecursively();
        return makeUnexpected(error);
    };

    QJsonArray manifestFiles;
    for (auto it = files.begin(); it != files.end(); ++it)
    {
        auto json = parseJson(it.value());
        if (!isRestorableFile(it.key()) || !json ||
            !isValidContent(it.key(), *json))
        {
            return fail(u"%1 can't be restored, it's damaged."_s.arg(it.key()));
        }

        const auto stagedName = it.key() + u".pending"_s;
        auto written = writeFile(combinePath(staging, stagedName), it.value());
        if (!written)
        {
            return fail(written.error());
        }
        manifestFiles.append(QJsonObject{
            {u"staged"_s, stagedName},
            {u"target"_s, it.key()},
        });
    }

    const QJsonObject manifest{
        {u"version"_s, 1},
        {u"createdAt"_s,
         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {u"files"_s, manifestFiles},
    };
    auto written = writeFile(combinePath(staging, MANIFEST_FILE),
                             QJsonDocument(manifest).toJson());
    if (!written)
    {
        return fail(written.error());
    }
    return {};
}

/// Writes the files of a staged restore, keeping rescue copies of the files
/// they replace
ExpectedStr<void> writeStagedFiles(const QString &settingsDirectory,
                                   const QString &staging,
                                   const QString &manifestPath)
{
    const auto damaged = u"The staged restore is damaged."_s;

    auto manifestContents = readFile(manifestPath);
    if (!manifestContents)
    {
        return makeUnexpected(manifestContents.error());
    }
    auto manifest = parseJson(*manifestContents);
    if (!manifest || manifest->toObject().value(u"version"_s).toInt() != 1)
    {
        return makeUnexpected(damaged);
    }

    struct Pending {
        QString target;
        QString targetPath;
        QByteArray contents;
        /// Empty if there was nothing to replace
        QString rescuePath;
    };
    std::vector<Pending> pending;
    const auto files = manifest->toObject().value(u"files"_s).toArray();
    for (const auto &value : files)
    {
        const auto spec = value.toObject();
        const auto stagedName = spec.value(u"staged"_s).toString();
        const auto target = spec.value(u"target"_s).toString();
        const bool duplicate =
            std::ranges::any_of(pending, [&](const auto &file) {
                return file.target == target;
            });
        if (!isRestorableFile(target) || duplicate ||
            QFileInfo(stagedName).fileName() != stagedName ||
            !stagedName.endsWith(u".pending"_s))
        {
            return makeUnexpected(damaged);
        }
        auto contents = readFile(combinePath(staging, stagedName));
        if (!contents)
        {
            return makeUnexpected(contents.error());
        }
        auto json = parseJson(*contents);
        if (!json || !isValidContent(target, *json))
        {
            return makeUnexpected(damaged);
        }
        pending.push_back({
            .target = target,
            .targetPath = combinePath(settingsDirectory, target),
            .contents = *contents,
        });
    }
    if (pending.empty())
    {
        return makeUnexpected(damaged);
    }

    for (auto &file : pending)
    {
        if (!QFileInfo::exists(file.targetPath))
        {
            continue;
        }
        auto copied = makeRescueCopy(file.targetPath);
        if (!copied)
        {
            for (const auto &other : pending)
            {
                if (!other.rescuePath.isEmpty())
                {
                    QFile::remove(other.rescuePath);
                }
            }
            return makeUnexpected(copied.error());
        }
        file.rescuePath = *copied;
    }

    for (size_t i = 0; i < pending.size(); ++i)
    {
        auto written = writeFile(pending[i].targetPath, pending[i].contents);
        if (written)
        {
            continue;
        }

        // put back what was already written
        bool rolledBack = true;
        for (size_t j = 0; j < i; ++j)
        {
            const auto &file = pending[j];
            if (file.rescuePath.isEmpty())
            {
                rolledBack &= QFile::remove(file.targetPath);
                continue;
            }
            auto original = readFile(file.rescuePath);
            rolledBack &=
                original && writeFile(file.targetPath, *original).has_value();
        }
        if (!rolledBack)
        {
            return makeUnexpected(
                QString(written.error() %
                        u" Some files were already restored, their rescue "
                        u"copies have what was there before."));
        }
        return makeUnexpected(written.error());
    }

    for (const auto &file : pending)
    {
        removeOldRescueCopies(settingsDirectory, file.target);
    }
    return {};
}

QString plural(qsizetype count, const QString &one, const QString &many)
{
    return u"%1 %2"_s.arg(count).arg(count == 1 ? one : many);
}

/// Counts the values in an object, descending into objects
qsizetype countValues(const QJsonValue &value)
{
    if (!value.isObject())
    {
        return 1;
    }
    qsizetype count = 0;
    const auto object = value.toObject();
    for (const auto &child : object)
    {
        count += countValues(child);
    }
    return count;
}

qsizetype countSplits(const QJsonValue &node)
{
    const auto object = node.toObject();
    if (object.value(u"type"_s).toString() == u"split"_s)
    {
        return 1;
    }
    qsizetype count = 0;
    const auto items = object.value(u"items"_s).toArray();
    for (const auto &child : items)
    {
        count += countSplits(child);
    }
    return count;
}

/// Describes what a settings file contains
QString summaryFor(const QString &file, const QJsonValue &value)
{
    const auto object = value.toObject();
    if (file == COMMANDS_FILE)
    {
        return plural(object.value(u"commands"_s).toArray().size(),
                      u"command"_s, u"commands"_s);
    }
    if (file == USER_DATA_FILE)
    {
        return plural(object.value(u"users"_s).toObject().size(), u"user"_s,
                      u"users"_s);
    }
    if (file == LAYOUT_FILE)
    {
        const auto windows = object.value(u"windows"_s).toArray();
        qsizetype tabs = 0;
        qsizetype splits = 0;
        for (const auto &window : windows)
        {
            const auto windowTabs =
                window.toObject().value(u"tabs"_s).toArray();
            tabs += windowTabs.size();
            for (const auto &tab : windowTabs)
            {
                splits += countSplits(tab.toObject().value(u"splits2"_s));
            }
        }
        QString summary = plural(tabs, u"tab"_s, u"tabs"_s) % u", " %
                          plural(splits, u"chat"_s, u"chats"_s);
        if (windows.size() > 1)
        {
            summary +=
                u", " % plural(windows.size(), u"window"_s, u"windows"_s);
        }
        return summary;
    }
    return plural(object.isEmpty() ? 0 : countValues(object), u"setting"_s,
                  u"settings"_s);
}

/// Returns the settings that would be applied live (path -> value), or why
/// the selection needs a restart
ExpectedStr<QJsonObject> liveValues(const Bundle &bundle,
                                    const QSet<QString> &selected)
{
    const auto needsRestart = u"This selection needs a restart."_s;
    if (selected.isEmpty())
    {
        return makeUnexpected(u"Choose something to import."_s);
    }
    for (const auto &category : selected)
    {
        if (!LIVE_CATEGORIES.contains(category))
        {
            return makeUnexpected(needsRestart);
        }
    }
    for (auto it = bundle.files.begin(); it != bundle.files.end(); ++it)
    {
        if (selected.contains(categoryForFile(it.key())))
        {
            return makeUnexpected(needsRestart);
        }
    }

    const auto registered = getSettings()->registeredValues();
    QJsonObject values;
    for (const auto &category : selected)
    {
        const auto group = bundle.settings.value(category);
        for (auto it = group.begin(); it != group.end(); ++it)
        {
            if (!isTransferableSettingPath(it.key()))
            {
                continue;
            }
            // a different type would only be fixed up by a migration
            const auto current = registered.constFind(it.key());
            if (RESTART_SETTINGS.contains(it.key().toLower()) ||
                (current != registered.constEnd() &&
                 current.value().type() != it.value().type()))
            {
                return makeUnexpected(needsRestart);
            }
            values.insert(it.key(), it.value());
        }
    }
    if (values.isEmpty())
    {
        return makeUnexpected(
            u"There's nothing to import in this selection."_s);
    }
    return values;
}

bool setLiveValue(const QString &path, const QJsonValue &value)
{
    const auto json =
        QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    rapidjson::Document document;
    document.Parse(json.constData(), static_cast<size_t>(json.size()));
    if (document.HasParseError() || !document.IsArray() || document.Size() != 1)
    {
        return false;
    }

    pajlada::Settings::SignalArgs args;
    args.source = pajlada::Settings::SignalArgs::Source::External;
    args.compareBeforeSet = true;
    pajlada::Settings::SettingManager::getInstance()->set(
        path.toStdString(), document[0u], std::move(args));
    return true;
}

}  // namespace

namespace chatterino::settingstransfer {

const std::vector<Category> &categories()
{
    static const std::vector<Category> values{
        {CATEGORY_APPEARANCE, u"Appearance"_s,
         u"Theme, fonts, messages, badges, and emotes"_s},
        {CATEGORY_CHAT, u"Chat and input"_s,
         u"Chat behavior, input, links, search, and translation"_s},
        {CATEGORY_LAYOUT, u"Tabs and layout"_s,
         u"Open windows, tabs, splits, and tab preferences"_s},
        {CATEGORY_HIGHLIGHTS, u"Highlights"_s,
         u"Highlighted messages, users, and badges"_s},
        {CATEGORY_COMMANDS, u"Hotkeys and commands"_s,
         u"Keyboard shortcuts and custom commands"_s},
        {CATEGORY_LISTS, u"Filters and personal lists"_s,
         u"Filters, ignores, nicknames, and muted channels"_s},
        {CATEGORY_MODERATION, u"Moderation"_s,
         u"Moderation tools, timeouts, logs, pins, polls, and predictions"_s},
        {CATEGORY_NOTIFICATIONS, u"Live notifications"_s,
         u"Which channels notify you when they go live, and how"_s},
        {CATEGORY_STREAMER_MODE, u"Streamer mode"_s,
         u"When streamer mode turns on and what it hides"_s},
        {CATEGORY_FEATURES, u"Extra features"_s,
         u"User cards and Leafyrino, Moltorino, and Technorino features"_s},
        {CATEGORY_PERSONAL, u"User notes and colors"_s,
         u"Notes and colors you gave to users"_s},
        {CATEGORY_ADVANCED, u"Advanced"_s,
         u"Connection and protocol options, experiments, and the sound "
         u"backend"_s},
        {CATEGORY_OTHER, u"Everything else"_s,
         u"Settings that don't fit in the other categories"_s},
    };
    return values;
}

QString categoryForSettingPath(const QString &path)
{
    const auto lower = path.toLower();
    const auto under = [&](std::initializer_list<QStringView> prefixes) {
        return std::ranges::any_of(prefixes, [&](QStringView prefix) {
            return isUnder(lower, prefix);
        });
    };
    const auto mentions = [&](std::initializer_list<QStringView> parts) {
        return containsAny(lower, parts);
    };

    if (under({u"/notifications"}))
    {
        return CATEGORY_NOTIFICATIONS;
    }
    if (under({u"/streamermode"}))
    {
        return CATEGORY_STREAMER_MODE;
    }
    if (under({u"/misc/chatsendprotocol", u"/misc/twitch/recentmessagesapi",
               u"/misc/x-7tv", u"/misc/extension", u"/x-chatterino7",
               u"/eventsub", u"/experiments", u"/sound"}))
    {
        return CATEGORY_ADVANCED;
    }

    // settings that don't fit where their path would put them
    if (under({u"/misc/displayseventvpaints",
               u"/misc/displayseventvpaintshadows",
               u"/misc/largeseventvpaintshadows",
               u"/misc/displayseventvanimatedprofile",
               u"/misc/emotestooltippreview", u"/twitch/gifs"}))
    {
        return CATEGORY_APPEARANCE;
    }
    if (under({u"/misc/twitch", u"/misc/scrollback",
               u"/misc/openlinksincognito", u"/misc/askonimageupload",
               u"/misc/showpronouns", u"/misc/hideemojibutton",
               u"/misc/markdownparsing", u"/misc/searchfilterspanelopen"}))
    {
        return CATEGORY_CHAT;
    }
    if (under({u"/appearance/tabs", u"/appearance/tabhighlightsusethemecolor",
               u"/misc/askontabvisibilitytoggle", u"/misc/locknotebooklayout",
               u"/misc/autodetachlivetab"}))
    {
        return CATEGORY_LAYOUT;
    }
    if (under({u"/misc/hidemodactionsonmodusercards"}))
    {
        return CATEGORY_MODERATION;
    }
    if (under({u"/moltorino/hiddenusers"}))
    {
        return CATEGORY_LISTS;
    }
    // Technorino's
    if (under({u"/misc/normalnoncedetection", u"/misc/abnormalnoncedetection",
               u"/misc/noncefuckeryenabled", u"/misc/fakewebchat",
               u"/misc/webchatcolor", u"/misc/androidcolor", u"/misc/ioscolor",
               u"/misc/mobilenewcolor", u"/misc/botlimitsjoin",
               u"/misc/botlimitsmessage", u"/misc/clientdetectionicon",
               u"/misc/clientdetectionshowbadge",
               u"/misc/watchingtablivesound"}))
    {
        return CATEGORY_FEATURES;
    }

    if (mentions({u"highlight", u"blacklisted"}))
    {
        return CATEGORY_HIGHLIGHTS;
    }
    if (under({u"/moltorino", u"/leafyrino", u"/usercard", u"/client"}))
    {
        return mentions({u"moder", u"nuke", u"spam", u"pyramid", u"repeat",
                         u"pinned", u"poll", u"prediction", u"logs", u"editor",
                         u"leadmod", u"automod", u"modcommand", u"modaction",
                         u"rolemanagement"})
                   ? CATEGORY_MODERATION
                   : CATEGORY_FEATURES;
    }
    if (mentions({u"hotkey", u"command"}))
    {
        return CATEGORY_COMMANDS;
    }
    if (mentions({u"filter", u"ignore", u"nickname", u"muted"}))
    {
        return CATEGORY_LISTS;
    }
    if (under({u"/appearance", u"/emotes", u"/theme", u"/seventv"}))
    {
        return mentions({u"tab"}) ? CATEGORY_LAYOUT : CATEGORY_APPEARANCE;
    }
    if (mentions({u"tab", u"windowlayout"}))
    {
        return CATEGORY_LAYOUT;
    }
    if (under({u"/moderation", u"/timeouts", u"/logging"}))
    {
        return CATEGORY_MODERATION;
    }
    if (under(
            {u"/behaviour", u"/links", u"/similarity", u"/whispers", u"/ui"}) ||
        mentions({u"translat", u"input"}))
    {
        return CATEGORY_CHAT;
    }
    return CATEGORY_OTHER;
}

bool isTransferableSettingPath(const QString &path)
{
    if (!path.startsWith(u'/') || path.endsWith(u'/') || path.size() > 512 ||
        path.contains(u"//"))
    {
        return false;
    }
    const auto segments = path.split(u'/');
    if (segments.size() > 33 ||
        std::ranges::any_of(segments, [](const QString &segment) {
            return segment == u"."_s || segment == u".."_s;
        }))
    {
        return false;
    }

    // accounts and secrets, privacy choices, state that belongs to this
    // install, and settings that make Leafyrino start programs
    static constexpr QStringView BLOCKED[] = {
        u"/accounts",
        u"/kickaccounts",
        u"/youtubeaccounts",
        u"/tiktokaccounts",
        u"/accountcredentials",
        u"/moltorino/auth",
        u"/moltorino/botbadge",
        u"/moltorino/presence",
        u"/moltorino/vanity/seventv",
        u"/moltorino/client/runtime",
        u"/moltorino/client/sendactivityheartbeats",
        u"/moltorino/client/hideaccountinheartbeats",
        u"/moltorino/client/sendheartbeattelemetry",
        u"/moltorino/client/anonymizeheartbeattelemetry",
        u"/moltorino/dailymessage/lastshowndate",
        u"/moltorino/dailymessage/lastquote",
        u"/moltorino/dailymessage/lastleadin",
        u"/external/imageuploader",
        u"/external/streamlink/options",
        u"/external/urischeme",
        u"/plugins",
        u"/proxy",
        u"/misc/settingsversion",
        u"/misc/currentversion",
        u"/leafyrino/lastseenbuild",
        u"/misc/usekeyring",
        u"/misc/additionalextensionids",
        u"/misc/attachextensiontoanyprocess",
        u"/behaviour/autorun",
    };
    const auto lower = path.toLower();
    for (auto blocked : BLOCKED)
    {
        // a parent would carry the blocked setting with it
        if (isUnder(lower, blocked) || isUnder(blocked, lower))
        {
            return false;
        }
    }

    return !containsAny(
        lower, {u"token", u"secret", u"password", u"credential", u"cookie",
                u"oauth", u"clientid", u"apikey", u"api_key", u"authorization",
                u"customsound", u"soundurl", u"path"});
}

QSet<QString> Bundle::categories() const
{
    QSet<QString> result;
    for (auto it = this->settings.begin(); it != this->settings.end(); ++it)
    {
        if (!it->isEmpty())
        {
            result.insert(it.key());
        }
    }
    for (auto it = this->files.begin(); it != this->files.end(); ++it)
    {
        result.insert(categoryForFile(it.key()));
    }
    return result;
}

ExpectedStr<QByteArray> exportSettings(const QString &settingsDirectory,
                                       const QSet<QString> &selected)
{
    auto stored = readSettingsFile(settingsDirectory, SETTINGS_FILE);
    if (!stored)
    {
        return makeUnexpected(stored.error());
    }

    const auto registered = getSettings()->registeredValues();
    QJsonObject flat;
    flattenSettings(*stored, {}, registered, flat);
    // in case something changed since the last save
    for (auto it = registered.begin(); it != registered.end(); ++it)
    {
        if (!flat.contains(it.key()))
        {
            flat.insert(it.key(), it.value());
        }
    }

    QHash<QString, QJsonObject> grouped;
    for (auto it = flat.begin(); it != flat.end(); ++it)
    {
        if (!isTransferableSettingPath(it.key()))
        {
            continue;
        }
        const auto category = categoryForSettingPath(it.key());
        if (selected.contains(category))
        {
            grouped[category].insert(it.key(), withoutPrivateKeys(it.value()));
        }
    }
    QJsonObject settings;
    for (auto it = grouped.begin(); it != grouped.end(); ++it)
    {
        settings.insert(it.key(), it.value());
    }

    QJsonObject files;
    for (const auto &file : TRANSFER_FILES)
    {
        if (!selected.contains(file.category) ||
            !QFileInfo::exists(combinePath(settingsDirectory, file.name)))
        {
            continue;
        }
        auto data = readSettingsFile(settingsDirectory, file.name);
        if (!data)
        {
            return makeUnexpected(data.error());
        }
        files.insert(file.key, QJsonObject{
                                   {u"category"_s, file.category},
                                   {u"target"_s, file.name},
                                   {u"data"_s, withoutPrivateKeys(*data)},
                               });
    }

    if (settings.isEmpty() && files.isEmpty())
    {
        return makeUnexpected(
            u"There's nothing to export in this selection."_s);
    }

    QJsonArray categoryIDs;
    for (const auto &category : categories())
    {
        if (selected.contains(category.id))
        {
            categoryIDs.append(category.id);
        }
    }
    const QJsonObject root{
        {u"format"_s, FORMAT_NAME},
        {u"version"_s, FORMAT_VERSION},
        {u"createdAt"_s,
         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {u"categories"_s, categoryIDs},
        {u"settings"_s, settings},
        {u"files"_s, files},
    };
    auto contents = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (contents.size() > MAX_FILE_SIZE)
    {
        return makeUnexpected(u"These settings are too large to export."_s);
    }
    return contents;
}

ExpectedStr<Bundle> parseBundle(const QByteArray &contents)
{
    const auto notSettings = u"This isn't a Leafyrino settings file."_s;
    if (contents.size() > MAX_FILE_SIZE)
    {
        return makeUnexpected(notSettings);
    }
    auto json = parseJson(contents);
    const auto root = json ? json->toObject() : QJsonObject{};
    const auto format = root.value(u"format"_s).toString();
    if (format != FORMAT_NAME && format != MOLTORINO_FORMAT_NAME)
    {
        return makeUnexpected(notSettings);
    }
    const auto version = root.value(u"version"_s).toInt();
    if (version > FORMAT_VERSION)
    {
        return makeUnexpected(
            u"This settings file is from a newer version of Leafyrino."_s);
    }
    if (version < 1)
    {
        return makeUnexpected(notSettings);
    }

    Bundle bundle;
    // the file can be from a version that sorts settings differently
    const auto groups = root.value(u"settings"_s).toObject();
    for (auto group = groups.begin(); group != groups.end(); ++group)
    {
        const auto values = group.value().toObject();
        for (auto it = values.begin(); it != values.end(); ++it)
        {
            if (isTransferableSettingPath(it.key()))
            {
                bundle.settings[categoryForSettingPath(it.key())].insert(
                    it.key(), withoutPrivateKeys(it.value()));
            }
        }
    }

    const auto files = root.value(u"files"_s).toObject();
    for (auto it = files.begin(); it != files.end(); ++it)
    {
        const auto file = it.value().toObject();
        const auto target = file.value(u"target"_s).toString();
        const auto data = withoutPrivateKeys(file.value(u"data"_s));
        // skips files that only other versions have
        if (!categoryForFile(target).isEmpty() && isValidContent(target, data))
        {
            bundle.files.insert(target, data);
        }
    }

    if (bundle.settings.isEmpty() && bundle.files.isEmpty())
    {
        return makeUnexpected(
            u"This settings file has nothing that can be imported."_s);
    }
    return bundle;
}

ExpectedStr<void> stageImport(const QString &settingsDirectory,
                              const Bundle &bundle,
                              const QSet<QString> &selected, bool mergeLists)
{
    QHash<QString, QByteArray> staged;

    auto stored = readSettingsFile(settingsDirectory, SETTINGS_FILE);
    if (!stored)
    {
        return makeUnexpected(stored.error());
    }
    auto settings = stored->toObject();
    bool settingsChanged = false;
    for (auto group = bundle.settings.begin(); group != bundle.settings.end();
         ++group)
    {
        if (!selected.contains(group.key()))
        {
            continue;
        }
        for (auto it = group->begin(); it != group->end(); ++it)
        {
            if (!isTransferableSettingPath(it.key()))
            {
                continue;
            }
            setAtPath(settings, it.key().split(u'/', Qt::SkipEmptyParts), 0,
                      it.value(), mergeLists);
            settingsChanged = true;
        }
    }
    if (settingsChanged)
    {
        staged.insert(SETTINGS_FILE, toJson(settings));
    }

    for (auto it = bundle.files.begin(); it != bundle.files.end(); ++it)
    {
        const auto &file = it.key();
        if (!selected.contains(categoryForFile(file)) ||
            !isValidContent(file, it.value()))
        {
            continue;
        }

        auto data = it.value();
        // a layout is replaced as a whole
        if (mergeLists && file != LAYOUT_FILE)
        {
            auto current = readSettingsFile(settingsDirectory, file);
            if (!current)
            {
                return makeUnexpected(
                    QString(current.error() %
                            u". Import it without merging to replace it."));
            }
            data = mergedValue(*current, data, true);
        }
        staged.insert(file, toJson(data));
    }

    if (staged.isEmpty())
    {
        return makeUnexpected(
            u"There's nothing to import in this selection."_s);
    }
    return stageFiles(settingsDirectory, staged);
}

bool canApplyImportNow(const Bundle &bundle, const QSet<QString> &selected)
{
    return liveValues(bundle, selected).has_value();
}

ExpectedStr<void> applyImportNow(const QString &settingsDirectory,
                                 const Bundle &bundle,
                                 const QSet<QString> &selected, bool mergeLists)
{
    auto values = liveValues(bundle, selected);
    if (!values)
    {
        return makeUnexpected(values.error());
    }
    auto stored = readSettingsFile(settingsDirectory, SETTINGS_FILE);
    if (!stored)
    {
        return makeUnexpected(stored.error());
    }

    // like a staged import, so it can be undone from the recovery tab
    const auto settingsPath = combinePath(settingsDirectory, SETTINGS_FILE);
    if (QFileInfo::exists(settingsPath))
    {
        auto copied = makeRescueCopy(settingsPath);
        if (!copied)
        {
            return makeUnexpected(copied.error());
        }
        removeOldRescueCopies(settingsDirectory, SETTINGS_FILE);
    }

    const auto current = stored->toObject();
    for (auto it = values->begin(); it != values->end(); ++it)
    {
        const auto value =
            mergedValue(valueAtPath(current, it.key()), it.value(), mergeLists);
        if (!setLiveValue(it.key(), value))
        {
            return makeUnexpected(u"Couldn't apply %1."_s.arg(it.key()));
        }
    }
    getSettings()->requestSave();
    return {};
}

bool RecoveryFile::isCurrent() const
{
    return QFileInfo(this->path).fileName() == this->targetFile;
}

bool RecoveryFile::isRescueCopy() const
{
    return QFileInfo(this->path)
        .fileName()
        .startsWith(this->targetFile % RESCUE_INFIX);
}

const std::vector<RecoveryTarget> &recoveryTargets()
{
    static const std::vector<RecoveryTarget> targets{
        {SETTINGS_FILE, u"Settings"_s},
        {LAYOUT_FILE, u"Tabs and layout"_s},
        {COMMANDS_FILE, u"Commands"_s},
        {USER_DATA_FILE, u"User notes and colors"_s},
    };
    return targets;
}

std::vector<RecoveryFile> inspectRecoveryFiles(const QString &settingsDirectory)
{
    std::vector<RecoveryFile> result;
    const QDir directory(settingsDirectory);
    for (const auto &target : recoveryTargets())
    {
        const auto entries = directory.entryInfoList(
            {target.file, target.file % BACKUP_INFIX % u'*',
             target.file % RESCUE_INFIX % u'*'},
            QDir::Files, QDir::Time);
        for (const auto &entry : entries)
        {
            RecoveryFile file{
                .path = entry.absoluteFilePath(),
                .targetFile = target.file,
                .lastModified = entry.lastModified(),
                .size = entry.size(),
            };

            auto contents = readFile(file.path);
            if (!contents)
            {
                file.state = RecoveryState::Unreadable;
                file.summary = u"Couldn't read the file"_s;
                result.push_back(std::move(file));
                continue;
            }

            auto json = parseJson(*contents);
            if (contents->trimmed().isEmpty())
            {
                file.state = RecoveryState::Damaged;
                file.summary = u"The file is empty"_s;
            }
            else if (!json || !isValidContent(target.file, *json))
            {
                file.state = RecoveryState::Damaged;
                file.summary = u"The file is damaged"_s;
            }
            else
            {
                file.summary = summaryFor(target.file, *json);
                file.state = json->toObject().isEmpty()
                                 ? RecoveryState::Empty
                                 : RecoveryState::Healthy;
            }
            result.push_back(std::move(file));
        }
    }
    return result;
}

ExpectedStr<void> stageRecovery(const QString &settingsDirectory,
                                const std::vector<RecoveryFile> &files)
{
    const auto directory = QDir(settingsDirectory).absolutePath();
    QHash<QString, QByteArray> contents;
    for (const auto &file : files)
    {
        const QFileInfo info(file.path);
        const auto name = info.fileName();
        if (!isRestorableFile(file.targetFile) ||
            contents.contains(file.targetFile) ||
            info.absolutePath() != directory ||
            (!name.startsWith(file.targetFile % BACKUP_INFIX) &&
             !name.startsWith(file.targetFile % RESCUE_INFIX)))
        {
            return makeUnexpected(
                u"Choose one backup for each file you want to restore."_s);
        }
        auto fileContents = readFile(file.path);
        if (!fileContents)
        {
            return makeUnexpected(fileContents.error());
        }
        contents.insert(file.targetFile, *fileContents);
    }
    return stageFiles(settingsDirectory, contents);
}

ExpectedStr<void> applyPendingRestore(const QString &settingsDirectory)
{
    const auto staging = combinePath(settingsDirectory, STAGING_DIRECTORY);
    if (!QFileInfo::exists(staging))
    {
        return {};
    }
    const auto manifestPath = combinePath(staging, MANIFEST_FILE);
    if (!QFileInfo::exists(manifestPath))
    {
        // staging didn't finish
        QDir(staging).removeRecursively();
        return {};
    }

    auto result = writeStagedFiles(settingsDirectory, staging, manifestPath);

    // a restore is only tried once
    const bool removed =
        QFile::remove(manifestPath) || !QFileInfo::exists(manifestPath);
    QDir(staging).removeRecursively();
    if (!result)
    {
        return result;
    }
    if (!removed)
    {
        return makeUnexpected(
            u"Couldn't remove %1. Delete it before Leafyrino starts again, or "
            u"the restore will be done again."_s.arg(manifestPath));
    }
    return {};
}

QString recoveryStateText(RecoveryState state)
{
    switch (state)
    {
        case RecoveryState::Healthy:
            return u"Ready"_s;
        case RecoveryState::Empty:
            return u"Empty"_s;
        case RecoveryState::Damaged:
            return u"Damaged"_s;
        case RecoveryState::Unreadable:
            return u"Unreadable"_s;
    }
    return u"Unknown"_s;
}

}  // namespace chatterino::settingstransfer

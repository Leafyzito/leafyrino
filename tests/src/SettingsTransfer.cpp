// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/SettingsTransfer.hpp"

#include "mocks/BaseApplication.hpp"
#include "singletons/Settings.hpp"
#include "Test.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>

using namespace chatterino;
using namespace chatterino::settingstransfer;
using namespace Qt::StringLiterals;

namespace {

void writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(contents);
}

void writeJson(const QString &path, const QJsonObject &object)
{
    writeFile(path, QJsonDocument(object).toJson());
}

QJsonObject readJson(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

/// Returns the value at a setting path like /appearance/theme/name
QJsonValue at(const QJsonObject &root, const QString &path)
{
    QJsonValue current = root;
    for (const auto &segment : path.split('/', Qt::SkipEmptyParts))
    {
        current = current.toObject().value(segment);
    }
    return current;
}

QByteArray makeBundle(const QJsonObject &settings,
                      const QJsonObject &files = {},
                      const QString &format = u"leafyrino-settings"_s,
                      int version = 1)
{
    return QJsonDocument(QJsonObject{
                             {"format", format},
                             {"version", version},
                             {"settings", settings},
                             {"files", files},
                         })
        .toJson();
}

QJsonObject fileEntry(const QString &target, const QJsonObject &data)
{
    return {{"target", target}, {"data", data}};
}

QJsonObject layout(const QJsonArray &tabs)
{
    return {{"windows", QJsonArray{QJsonObject{{"tabs", tabs}}}}};
}

QJsonObject splitTab()
{
    return {{"splits2", QJsonObject{{"type", "split"}}}};
}

QSet<QString> allCategories()
{
    QSet<QString> ids;
    for (const auto &category : categories())
    {
        ids.insert(category.id);
    }
    return ids;
}

qsizetype countFiles(const QTemporaryDir &dir, const QString &pattern)
{
    return QDir(dir.path()).entryList({pattern}, QDir::Files).size();
}

}  // namespace

TEST(SettingsTransfer, TransferablePaths)
{
    for (const auto *path : {
             "/accounts",
             "/accounts/uid123/oauthToken",
             "/kickAccounts/current",
             "/external/streamlink/options",
             "/external/urischeme",
             "/external/imageUploader/headers",
             "/external",
             "/plugins/enabledPlugins",
             "/misc/settingsVersion",
             "/leafyrino/lastSeenBuild",
             "/misc/useKeyring",
             "/behaviour/autorun",
             "/logging/path",
             "/notifications/highlightSoundPath",
             "/highlighting/subHighlightSoundUrl",
             "/moltorino/pinnedMessages/customAuthToken",
             "/moltorino/client/runtime",
             "no-slash",
             "/trailing/",
             "/double//slash",
             "/a/../b",
         })
    {
        EXPECT_FALSE(isTransferableSettingPath(QString::fromUtf8(path)))
            << path;
    }

    for (const auto *path : {
             "/appearance/theme/name",
             "/external/streamlink/quality",
             "/highlighting/highlights2",
             "/hotkeys/popup accept/keySequence",
             "/moltorino/client/translationProvider",
         })
    {
        EXPECT_TRUE(isTransferableSettingPath(QString::fromUtf8(path))) << path;
    }
}

TEST(SettingsTransfer, Categories)
{
    const std::pair<const char *, const char *> cases[] = {
        {"/appearance/theme/name", "appearance"},
        {"/emotes/scale", "appearance"},
        {"/appearance/tabStyle", "layout"},
        {"/appearance/tabs/colorHighlightsByMessage", "layout"},
        {"/highlighting/highlights2", "highlights"},
        {"/hotkeys/popup accept/action", "commands"},
        {"/filtering/filters", "lists"},
        {"/nicknames", "lists"},
        {"/moltorino/pinnedMessages/enabled", "moderation"},
        {"/moltorino/polls/enabled", "moderation"},
        {"/timeouts/timeoutButtons", "moderation"},
        {"/usercard/showFollowage", "features"},
        {"/leafyrino/includeSupibotCommands", "features"},
        {"/misc/fakeWebChat", "features"},
        {"/links/linkInfoTooltip", "chat"},
        {"/misc/twitch/messageHistoryLimit", "chat"},
        {"/twitch/gifs/favorites", "appearance"},
        {"/notifications/enableToast", "notifications"},
        {"/notifications/twitch", "notifications"},
        {"/streamerMode/enabled", "streamer-mode"},
        {"/streamerMode/hideModActions", "streamer-mode"},
        {"/misc/chatSendProtocol", "advanced"},
        {"/misc/twitch/recentMessagesApi", "advanced"},
        {"/misc/x-7tv/twitchReadConnectionMode", "advanced"},
        {"/eventsub/enableExperimental", "advanced"},
        {"/sound/backend", "advanced"},
        {"/external/streamlink/quality", "other"},
    };
    const auto ids = allCategories();
    for (const auto &[path, category] : cases)
    {
        const auto actual = categoryForSettingPath(QString::fromUtf8(path));
        EXPECT_EQ(actual, QString::fromUtf8(category)) << path;
        EXPECT_TRUE(ids.contains(actual)) << path;
    }
}

TEST(SettingsTransfer, ExportLeavesOutPrivateSettings)
{
    mock::BaseApplication app(uR"({
        "appearance": {"theme": {"name": "White"}},
        "accounts": {"current": "me", "uid1": {"oauthToken": "hunter2"}},
        "external": {
            "streamlink": {"options": "--player calc", "quality": "best"},
            "urischeme": "evil://"
        },
        "highlighting": {
            "highlights": [{"pattern": "hi", "soundUrl": "file:///x.wav"}]
        },
        "hotkeys": {"my hotkey": {"action": "openSettings"}}
    })"_s);
    const auto dir = app.settingsDir.path();
    writeJson(dir + "/commands.json",
              {{"commands",
                QJsonArray{QJsonObject{{"name", "/hi"}, {"func", "hello"}}}}});

    auto exported = exportSettings(dir, allCategories());
    ASSERT_TRUE(exported) << exported.error();
    const auto text = QString::fromUtf8(*exported);
    EXPECT_FALSE(text.contains("hunter2"));
    EXPECT_FALSE(text.contains("--player"));
    EXPECT_FALSE(text.contains("evil://"));
    EXPECT_FALSE(text.contains("x.wav"));

    auto bundle = parseBundle(*exported);
    ASSERT_TRUE(bundle) << bundle.error();
    EXPECT_EQ(
        bundle->settings.value("appearance").value("/appearance/theme/name"),
        QJsonValue("White"));
    EXPECT_EQ(
        bundle->settings.value("other").value("/external/streamlink/quality"),
        QJsonValue("best"));
    EXPECT_TRUE(bundle->settings.value("commands")
                    .contains("/hotkeys/my hotkey/action"));
    EXPECT_TRUE(bundle->files.contains("commands.json"));

    // the highlight is kept, just without its sound
    const auto highlights = bundle->settings.value("highlights")
                                .value("/highlighting/highlights")
                                .toArray();
    ASSERT_EQ(highlights.size(), 1);
    EXPECT_EQ(highlights[0].toObject().value("pattern"), QJsonValue("hi"));
    EXPECT_FALSE(highlights[0].toObject().contains("soundUrl"));

    // only what was selected
    auto appearance = exportSettings(dir, {"appearance"});
    ASSERT_TRUE(appearance);
    auto appearanceBundle = parseBundle(*appearance);
    ASSERT_TRUE(appearanceBundle);
    EXPECT_EQ(appearanceBundle->categories(), QSet<QString>{"appearance"});

    EXPECT_FALSE(exportSettings(dir, {}));
}

TEST(SettingsTransfer, ParseSortsAndFilters)
{
    // Moltorino's chat automations and the empty layout can't be imported,
    // and the settings are in categories this version doesn't use
    auto bundle = parseBundle(makeBundle(
        {
            {"appearance",
             QJsonObject{
                 {"/highlighting/highlights2", QJsonArray{}},
                 {"/external/urischeme", "evil://"},
                 {"/appearance/theme/name", "Dark"},
             }},
            {"moltorino-general",
             QJsonObject{{"/links/linkInfoTooltip", true}}},
        },
        {
            {"automations",
             fileEntry("chat-automations.json", {{"rules", QJsonArray{}}})},
            {"layout",
             fileEntry("window-layout.json", {{"windows", QJsonArray{}}})},
            {"commands", fileEntry("commands.json", {{"commands", QJsonArray{}},
                                                     {"apiKey", "hunter2"}})},
        },
        u"moltorino-settings"_s));
    ASSERT_TRUE(bundle) << bundle.error();

    EXPECT_TRUE(bundle->settings.value("highlights")
                    .contains("/highlighting/highlights2"));
    EXPECT_TRUE(
        bundle->settings.value("chat").contains("/links/linkInfoTooltip"));
    EXPECT_EQ(bundle->settings.value("appearance").keys(),
              QStringList{"/appearance/theme/name"});
    EXPECT_EQ(bundle->files.keys(), QStringList{"commands.json"});
    EXPECT_FALSE(
        bundle->files.value("commands.json").toObject().contains("apiKey"));
    EXPECT_EQ(bundle->categories(),
              (QSet<QString>{"highlights", "chat", "appearance", "commands"}));

    const QJsonObject valid{
        {"chat", QJsonObject{{"/links/linkInfoTooltip", true}}},
    };
    EXPECT_TRUE(parseBundle(makeBundle(valid)));
    EXPECT_FALSE(parseBundle("not json"));
    EXPECT_FALSE(parseBundle(makeBundle(valid, {}, u"something-else"_s)));
    EXPECT_FALSE(
        parseBundle(makeBundle(valid, {}, u"leafyrino-settings"_s, 2)));
    EXPECT_FALSE(
        parseBundle(makeBundle(valid, {}, u"leafyrino-settings"_s, 0)));
    // nothing left to import
    EXPECT_FALSE(parseBundle(makeBundle(
        {{"other", QJsonObject{{"/accounts/current", "someone"}}}})));
}

TEST(SettingsTransfer, ImportMergesLists)
{
    QTemporaryDir dir;
    writeJson(dir.filePath("settings.json"),
              {
                  {"appearance",
                   QJsonObject{{"theme", QJsonObject{{"name", "White"}}}}},
                  {"accounts", QJsonObject{{"current", "me"}}},
                  {"highlighting",
                   QJsonObject{{"highlights",
                                QJsonArray{QJsonObject{{"pattern", "mine"}}}}}},
              });
    writeJson(dir.filePath("commands.json"),
              {{"commands", QJsonArray{
                                QJsonObject{{"name", "/a"}, {"func", "old"}},
                                QJsonObject{{"name", "/b"}, {"func", "kept"}},
                            }}});

    auto bundle = parseBundle(makeBundle(
        {
            {"appearance", QJsonObject{{"/appearance/theme/name", "Dark"}}},
            {"highlights", QJsonObject{{"/highlighting/highlights",
                                        QJsonArray{
                                            QJsonObject{{"pattern", "theirs"}},
                                            QJsonObject{{"pattern", "MINE"}},
                                        }}}},
        },
        {{"commands",
          fileEntry(
              "commands.json",
              {{"commands", QJsonArray{
                                QJsonObject{{"name", "/a"}, {"func", "new"}},
                                QJsonObject{{"name", "/c"}, {"func", "added"}},
                            }}})}}));
    ASSERT_TRUE(bundle) << bundle.error();

    auto staged = stageImport(dir.path(), *bundle,
                              {"appearance", "highlights", "commands"}, true);
    ASSERT_TRUE(staged) << staged.error();

    // nothing changes until the next start
    EXPECT_EQ(
        at(readJson(dir.filePath("settings.json")), "/appearance/theme/name"),
        QJsonValue("White"));

    auto applied = applyPendingRestore(dir.path());
    ASSERT_TRUE(applied) << applied.error();

    const auto settings = readJson(dir.filePath("settings.json"));
    EXPECT_EQ(at(settings, "/appearance/theme/name"), QJsonValue("Dark"));
    EXPECT_EQ(at(settings, "/accounts/current"), QJsonValue("me"));

    // MINE is the same highlight as mine, it's not case sensitive
    const auto highlights = at(settings, "/highlighting/highlights").toArray();
    ASSERT_EQ(highlights.size(), 2);
    EXPECT_EQ(highlights[0].toObject().value("pattern"), QJsonValue("MINE"));
    EXPECT_EQ(highlights[1].toObject().value("pattern"), QJsonValue("theirs"));

    const auto commands =
        readJson(dir.filePath("commands.json")).value("commands").toArray();
    ASSERT_EQ(commands.size(), 3);
    EXPECT_EQ(commands[0].toObject().value("func"), QJsonValue("new"));
    EXPECT_EQ(commands[1].toObject().value("func"), QJsonValue("kept"));
    EXPECT_EQ(commands[2].toObject().value("func"), QJsonValue("added"));

    // the replaced files are kept, and a restore only happens once
    EXPECT_EQ(countFiles(dir, "settings.json.rescue-*"), 1);
    EXPECT_EQ(countFiles(dir, "commands.json.rescue-*"), 1);
    EXPECT_FALSE(QFileInfo::exists(dir.filePath(".leafyrino-restore")));
    ASSERT_TRUE(applyPendingRestore(dir.path()));
    EXPECT_EQ(countFiles(dir, "settings.json.rescue-*"), 1);
}

TEST(SettingsTransfer, ImportReplacesLists)
{
    QTemporaryDir dir;
    writeJson(dir.filePath("settings.json"),
              {{"highlighting",
                QJsonObject{{"highlights",
                             QJsonArray{QJsonObject{{"pattern", "mine"}}}}}}});

    auto bundle = parseBundle(makeBundle(
        {{"highlights",
          QJsonObject{{"/highlighting/highlights",
                       QJsonArray{QJsonObject{{"pattern", "theirs"}}}}}}}));
    ASSERT_TRUE(bundle);

    // categories that weren't picked aren't imported
    EXPECT_FALSE(stageImport(dir.path(), *bundle, {"appearance"}, false));

    ASSERT_TRUE(stageImport(dir.path(), *bundle, {"highlights"}, false));
    ASSERT_TRUE(applyPendingRestore(dir.path()));
    EXPECT_EQ(
        at(readJson(dir.filePath("settings.json")), "/highlighting/highlights"),
        QJsonValue(QJsonArray{QJsonObject{{"pattern", "theirs"}}}));
}

TEST(SettingsTransfer, Recovery)
{
    QTemporaryDir dir;
    writeJson(dir.filePath("window-layout.json"), layout({splitTab()}));
    writeJson(dir.filePath("window-layout.json.bkp-1"),
              layout({
                  splitTab(),
                  QJsonObject{{"splits2",
                               QJsonObject{
                                   {"type", "horizontal"},
                                   {"items",
                                    QJsonArray{
                                        QJsonObject{{"type", "split"}},
                                        QJsonObject{{"type", "split"}},
                                    }},
                               }}},
              }));
    writeFile(dir.filePath("window-layout.json.bkp-2"), "{ broken");

    const auto files = inspectRecoveryFiles(dir.path());
    const auto find = [&](const QString &name) {
        auto it = std::ranges::find_if(files, [&](const auto &file) {
            return QFileInfo(file.path).fileName() == name;
        });
        return it == files.end() ? RecoveryFile{} : *it;
    };

    const auto current = find("window-layout.json");
    EXPECT_TRUE(current.isCurrent());
    EXPECT_EQ(current.state, RecoveryState::Healthy);
    EXPECT_EQ(current.summary, "1 tab, 1 chat");

    const auto backup = find("window-layout.json.bkp-1");
    EXPECT_FALSE(backup.isCurrent());
    EXPECT_FALSE(backup.isRescueCopy());
    EXPECT_EQ(backup.state, RecoveryState::Healthy);
    EXPECT_EQ(backup.summary, "2 tabs, 3 chats");

    EXPECT_EQ(find("window-layout.json.bkp-2").state, RecoveryState::Damaged);

    // only backups from the settings directory can be restored
    EXPECT_FALSE(stageRecovery(dir.path(), {current}));
    QTemporaryDir otherDir;
    auto outside = backup;
    outside.path = otherDir.filePath("window-layout.json.bkp-1");
    EXPECT_FALSE(stageRecovery(dir.path(), {outside}));

    auto staged = stageRecovery(dir.path(), {backup});
    ASSERT_TRUE(staged) << staged.error();
    ASSERT_TRUE(applyPendingRestore(dir.path()));
    EXPECT_EQ(readJson(dir.filePath("window-layout.json")),
              readJson(dir.filePath("window-layout.json.bkp-1")));

    // the layout from before is now a rescue copy that can be restored
    const auto after = inspectRecoveryFiles(dir.path());
    const auto rescue = std::ranges::find_if(after, [](const auto &file) {
        return file.isRescueCopy();
    });
    ASSERT_NE(rescue, after.end());
    EXPECT_EQ(rescue->summary, "1 tab, 1 chat");
}

TEST(SettingsTransfer, DamagedRestoreIsDiscarded)
{
    QTemporaryDir dir;
    writeJson(dir.filePath("settings.json"), {{"kept", true}});
    QDir(dir.path()).mkpath(".leafyrino-restore");
    writeJson(
        dir.filePath(".leafyrino-restore/pending.json"),
        {
            {"version", 1},
            {"files", QJsonArray{QJsonObject{{"staged", "../settings.json"},
                                             {"target", "settings.json"}}}},
        });

    EXPECT_FALSE(applyPendingRestore(dir.path()));
    EXPECT_EQ(readJson(dir.filePath("settings.json")),
              (QJsonObject{{"kept", true}}));
    EXPECT_FALSE(QFileInfo::exists(dir.filePath(".leafyrino-restore")));
    EXPECT_TRUE(applyPendingRestore(dir.path()));
}

TEST(SettingsTransfer, ApplyNow)
{
    mock::BaseApplication app(u"{}"_s);
    const auto dir = app.settingsDir.path();
    ASSERT_TRUE(app.settings.showTimestamps.getValue());

    auto bundle = parseBundle(makeBundle(
        {{"appearance",
          QJsonObject{{"/appearance/messages/showTimestamps", false}}}}));
    ASSERT_TRUE(bundle);
    EXPECT_TRUE(canApplyImportNow(*bundle, {"appearance"}));
    auto applied = applyImportNow(dir, *bundle, {"appearance"}, true);
    ASSERT_TRUE(applied) << applied.error();
    EXPECT_FALSE(app.settings.showTimestamps.getValue());

    // read on startup
    auto legacyScaling = parseBundle(makeBundle(
        {{"appearance", QJsonObject{{"/appearance/useLegacyScaling", true}}}}));
    ASSERT_TRUE(legacyScaling);
    EXPECT_FALSE(canApplyImportNow(*legacyScaling, {"appearance"}));

    auto wrongType = parseBundle(makeBundle(
        {{"appearance",
          QJsonObject{{"/appearance/messages/showTimestamps", "no"}}}}));
    ASSERT_TRUE(wrongType);
    EXPECT_FALSE(canApplyImportNow(*wrongType, {"appearance"}));

    auto chat = parseBundle(
        makeBundle({{"chat", QJsonObject{{"/links/linkInfoTooltip", true}}}}));
    ASSERT_TRUE(chat);
    EXPECT_FALSE(canApplyImportNow(*chat, {"chat"}));
}

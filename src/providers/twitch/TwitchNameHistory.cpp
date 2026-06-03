#include "providers/twitch/TwitchNameHistory.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "singletons/Paths.hpp"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QFile>
#include <QSaveFile>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <memory>
#include <utility>

namespace chatterino {
namespace {

constexpr QStringView NAME_HISTORY_API =
    u"https://logs.zonian.dev/namehistory/%1";
constexpr int NAME_HISTORY_TIMEOUT_MS = 30000;
constexpr int NAME_HISTORY_CACHE_VERSION = 1;
constexpr int NAME_HISTORY_CACHE_TTL_DAYS = 21;
constexpr QStringView NAME_HISTORY_CACHE_FILE = u"twitch-name-history.json";

QString cacheKeyForUserId(const QString &userId)
{
    return "id:" + userId.trimmed();
}

QString cacheKeyForLogin(const QString &login)
{
    return "login:" + normalizeTwitchNameHistoryLogin(login);
}

using TwitchNameHistoryPtr = std::shared_ptr<TwitchNameHistory>;

QHash<QString, TwitchNameHistoryPtr> &nameHistoryCache()
{
    static QHash<QString, TwitchNameHistoryPtr> cache;
    return cache;
}

bool &nameHistoryCacheLoaded()
{
    static bool loaded = false;
    return loaded;
}

QDateTime parseNameHistoryTimestamp(const QString &isoTimestamp)
{
    auto timestamp = QDateTime::fromString(isoTimestamp, Qt::ISODateWithMs);
    if (!timestamp.isValid())
    {
        timestamp = QDateTime::fromString(isoTimestamp, Qt::ISODate);
    }
    if (!timestamp.isValid() && isoTimestamp.contains('.'))
    {
        auto trimmed = isoTimestamp;
        const auto dotIndex = trimmed.indexOf('.');
        const auto zoneIndex = trimmed.indexOf('Z', dotIndex);
        if (zoneIndex > dotIndex + 4)
        {
            trimmed = trimmed.left(dotIndex + 4) + trimmed.mid(zoneIndex);
            timestamp = QDateTime::fromString(trimmed, Qt::ISODateWithMs);
        }
    }

    return timestamp;
}

QString formatNameHistoryDate(const QString &isoTimestamp)
{
    const auto timestamp = parseNameHistoryTimestamp(isoTimestamp);
    if (!timestamp.isValid())
    {
        return {};
    }

    return timestamp.toLocalTime().date().toString("MMM d, yyyy");
}

QString formatNameHistoryDateOrUnknown(const QString &isoTimestamp)
{
    const auto date = formatNameHistoryDate(isoTimestamp);
    return date.isEmpty() ? QStringLiteral("Unknown") : date;
}

bool historyMatchesExpectedLogin(const TwitchNameHistory &history,
                                 const QString &expectedCurrentLogin)
{
    const auto expected = normalizeTwitchNameHistoryLogin(expectedCurrentLogin);
    return expected.isEmpty() ||
           (!history.currentLogin.isEmpty() &&
            history.currentLogin.compare(expected, Qt::CaseInsensitive) == 0);
}

bool nameHistoryIsFresh(const TwitchNameHistory &history)
{
    if (!history.fetchedAt.isValid())
    {
        return false;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    if (history.fetchedAt > now.addSecs(5 * 60))
    {
        return false;
    }

    return history.fetchedAt.secsTo(now) <=
           NAME_HISTORY_CACHE_TTL_DAYS * 24 * 60 * 60;
}

TwitchNameHistory parseNameHistory(const QJsonArray &root,
                                   const QString &userId,
                                   const QString &requestedLogin)
{
    TwitchNameHistory result;
    result.userId = userId;
    result.fetchedAt = QDateTime::currentDateTimeUtc();

    QDateTime newestSeenAt;
    for (const auto &value : root)
    {
        const auto object = value.toObject();
        const auto login = normalizeTwitchNameHistoryLogin(
            object.value("user_login").toString());
        if (login.isEmpty())
        {
            continue;
        }

        const auto lastSeenAt = parseNameHistoryTimestamp(
            object.value("last_timestamp").toString());
        if (!newestSeenAt.isValid() ||
            (lastSeenAt.isValid() && lastSeenAt > newestSeenAt))
        {
            newestSeenAt = lastSeenAt;
            result.currentLogin = login;
        }
    }
    if (result.currentLogin.isEmpty())
    {
        result.currentLogin = normalizeTwitchNameHistoryLogin(requestedLogin);
    }

    const auto reserveCount =
        std::min<int>(root.size(), TWITCH_NAME_HISTORY_LIMIT);
    result.entries.reserve(static_cast<size_t>(reserveCount));

    bool addedCurrentEntry = false;
    for (auto i = root.size() - 1; i >= 0; --i)
    {
        const auto object = root.at(i).toObject();
        auto login = normalizeTwitchNameHistoryLogin(
            object.value("user_login").toString());
        if (login.isEmpty())
        {
            continue;
        }

        auto firstSeen = formatNameHistoryDateOrUnknown(
            object.value("first_timestamp").toString());
        const auto lastSeen = formatNameHistoryDateOrUnknown(
            object.value("last_timestamp").toString());

        const bool isCurrentLogin =
            login.compare(result.currentLogin, Qt::CaseInsensitive) == 0;
        if (isCurrentLogin && !addedCurrentEntry)
        {
            addedCurrentEntry = true;
            result.entries.push_back(
                {std::move(login), firstSeen, QStringLiteral("Present")});
        }
        else
        {
            if (i == 0)
            {
                firstSeen = QStringLiteral("Unknown");
            }
            result.entries.push_back({std::move(login), firstSeen, lastSeen});
        }

        if (static_cast<int>(result.entries.size()) >=
            TWITCH_NAME_HISTORY_LIMIT)
        {
            break;
        }
    }

    return result;
}

void insertNameHistory(const TwitchNameHistory &history)
{
    auto &cache = nameHistoryCache();
    auto shared = std::make_shared<TwitchNameHistory>(history);

    if (!history.userId.isEmpty())
    {
        cache.insert(cacheKeyForUserId(history.userId), shared);
    }
    if (!history.currentLogin.isEmpty())
    {
        cache.insert(cacheKeyForLogin(history.currentLogin), shared);
    }
    for (const auto &entry : history.entries)
    {
        if (!entry.login.isEmpty())
        {
            cache.insert(cacheKeyForLogin(entry.login), shared);
        }
    }
}

QString nameHistoryCachePath()
{
    return getApp()->getPaths().cacheFilePath(
        NAME_HISTORY_CACHE_FILE.toString());
}

TwitchNameHistory historyFromJson(const QJsonObject &object)
{
    TwitchNameHistory history;
    history.userId = object.value("userId").toString().trimmed();
    history.currentLogin =
        normalizeTwitchNameHistoryLogin(object.value("currentLogin").toString());
    history.fetchedAt =
        parseNameHistoryTimestamp(object.value("fetchedAt").toString());

    const auto entries = object.value("entries").toArray();
    history.entries.reserve(static_cast<size_t>(
        std::min<int>(entries.size(), TWITCH_NAME_HISTORY_LIMIT)));

    for (const auto &value : entries)
    {
        const auto entryObject = value.toObject();
        TwitchNameHistoryEntry entry{
            normalizeTwitchNameHistoryLogin(
                entryObject.value("login").toString()),
            entryObject.value("left").toString(),
            entryObject.value("right").toString(),
        };
        if (entry.login.isEmpty() || entry.leftText.isEmpty() ||
            entry.rightText.isEmpty())
        {
            continue;
        }

        history.entries.push_back(std::move(entry));
        if (static_cast<int>(history.entries.size()) >=
            TWITCH_NAME_HISTORY_LIMIT)
        {
            break;
        }
    }

    return history;
}

QJsonObject historyToJson(const TwitchNameHistory &history)
{
    QJsonArray entries;
    for (const auto &entry : history.entries)
    {
        QJsonObject entryObject;
        entryObject.insert("login", entry.login);
        entryObject.insert("left", entry.leftText);
        entryObject.insert("right", entry.rightText);
        entries.push_back(entryObject);
    }

    QJsonObject object;
    object.insert("userId", history.userId);
    object.insert("currentLogin", history.currentLogin);
    object.insert("fetchedAt",
                  history.fetchedAt.toUTC().toString(Qt::ISODateWithMs));
    object.insert("entries", entries);
    return object;
}

void loadNameHistoryCache()
{
    auto &loaded = nameHistoryCacheLoaded();
    if (loaded)
    {
        return;
    }
    loaded = true;

    QFile file(nameHistoryCachePath());
    if (!file.open(QIODevice::ReadOnly))
    {
        return;
    }

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        return;
    }

    const auto root = document.object();
    if (root.value("version").toInt() != NAME_HISTORY_CACHE_VERSION)
    {
        return;
    }

    for (const auto &value : root.value("histories").toArray())
    {
        auto history = historyFromJson(value.toObject());
        if ((history.userId.isEmpty() && history.currentLogin.isEmpty()) ||
            !nameHistoryIsFresh(history))
        {
            continue;
        }

        insertNameHistory(history);
    }
}

void saveNameHistoryCache()
{
    QSet<QString> savedKeys;
    QJsonArray histories;

    for (const auto &historyPtr : nameHistoryCache())
    {
        if (historyPtr == nullptr)
        {
            continue;
        }

        const auto &history = *historyPtr;
        if (!nameHistoryIsFresh(history))
        {
            continue;
        }

        const auto key = history.userId.isEmpty()
                             ? QStringLiteral("login:") + history.currentLogin
                             : QStringLiteral("id:") + history.userId;
        if (key.endsWith(':') || savedKeys.contains(key))
        {
            continue;
        }
        savedKeys.insert(key);
        histories.push_back(historyToJson(history));
    }

    QJsonObject root;
    root.insert("version", NAME_HISTORY_CACHE_VERSION);
    root.insert("histories", histories);

    QSaveFile file(nameHistoryCachePath());
    if (!file.open(QIODevice::WriteOnly))
    {
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.commit();
}

void cacheNameHistory(const TwitchNameHistory &history)
{
    loadNameHistoryCache();
    insertNameHistory(history);
    saveNameHistoryCache();
}

}  // namespace

QString normalizeTwitchNameHistoryLogin(const QString &login)
{
    return login.trimmed().toLower();
}

std::optional<TwitchNameHistory> getCachedTwitchNameHistory(
    const QString &userId, const QString &expectedCurrentLogin)
{
    loadNameHistoryCache();

    const auto trimmedUserId = userId.trimmed();
    const auto loginKey = cacheKeyForLogin(expectedCurrentLogin);

    auto &cache = nameHistoryCache();
    if (!trimmedUserId.isEmpty())
    {
        if (const auto it = cache.constFind(cacheKeyForUserId(trimmedUserId));
            it != cache.cend() && *it != nullptr &&
            nameHistoryIsFresh(**it) &&
            historyMatchesExpectedLogin(**it, expectedCurrentLogin))
        {
            return **it;
        }
    }
    if (!expectedCurrentLogin.trimmed().isEmpty())
    {
        if (const auto it = cache.constFind(loginKey);
            it != cache.cend() && *it != nullptr &&
            nameHistoryIsFresh(**it) &&
            historyMatchesExpectedLogin(**it, expectedCurrentLogin))
        {
            return **it;
        }
    }

    return std::nullopt;
}

void fetchTwitchNameHistoryByUserId(
    const QString &userId, const QString &requestedLogin,
    std::function<void(TwitchNameHistory)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    const auto trimmedUserId = userId.trimmed();
    if (trimmedUserId.isEmpty())
    {
        failureCallback("Missing user id");
        return;
    }

    const auto encodedUserId =
        QString::fromUtf8(QUrl::toPercentEncoding(trimmedUserId));
    const auto url = QUrl(NAME_HISTORY_API.toString().arg(encodedUserId));

    NetworkRequest(url)
        .timeout(NAME_HISTORY_TIMEOUT_MS)
        .followRedirects(true)
        .header("Accept", "application/json")
        .header("User-Agent", "Leafyrino")
        .onSuccess([trimmedUserId, requestedLogin,
                    successCallback = std::move(successCallback),
                    failureCallback](const NetworkResult &result) mutable {
            const auto rootValue = result.parseJsonValue();
            if (!rootValue.isArray())
            {
                failureCallback("Unexpected name history response");
                return;
            }

            const auto root = rootValue.toArray();
            if (root.isEmpty())
            {
                TwitchNameHistory empty;
                empty.userId = trimmedUserId;
                empty.currentLogin =
                    normalizeTwitchNameHistoryLogin(requestedLogin);
                empty.fetchedAt = QDateTime::currentDateTimeUtc();
                cacheNameHistory(empty);
                successCallback(std::move(empty));
                return;
            }

            auto history =
                parseNameHistory(root, trimmedUserId, requestedLogin);
            cacheNameHistory(history);
            successCallback(std::move(history));
        })
        .onError([failureCallback = std::move(failureCallback)](
                     const NetworkResult &result) {
            failureCallback(result.formatError());
        })
        .execute();
}

}  // namespace chatterino

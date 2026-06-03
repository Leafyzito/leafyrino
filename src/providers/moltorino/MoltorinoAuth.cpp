#include "providers/moltorino/MoltorinoAuth.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchAccountManager.hpp"
#include "singletons/Settings.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <memory>

namespace chatterino::MoltorinoAuth {
namespace {

constexpr int HELIX_MODERATED_CHANNEL_TIMEOUT_MS = 20 * 1000;
constexpr int MAX_HELIX_MODERATED_CHANNEL_PAGES = 100;
constexpr auto TWITCH_TV_CLIENT_ID = "ue6666qo983tsx6so1t0vnawi233wa";

QString normalizeToken(QString token)
{
    token = token.trimmed();

    while (!token.isEmpty())
    {
        const auto previous = token;

        if (token.size() >= 2 &&
            ((token.startsWith('"') && token.endsWith('"')) ||
             (token.startsWith('\'') && token.endsWith('\''))))
        {
            token = token.mid(1, token.size() - 2).trimmed();
        }

        if (token.startsWith("Authorization:", Qt::CaseInsensitive))
        {
            token = token.mid(QString("Authorization:").size()).trimmed();
        }
        if (token.startsWith("OAuth ", Qt::CaseInsensitive))
        {
            token = token.mid(QString("OAuth ").size()).trimmed();
        }
        if (token.startsWith("Bearer ", Qt::CaseInsensitive))
        {
            token = token.mid(QString("Bearer ").size()).trimmed();
        }
        if (token.startsWith("oauth:", Qt::CaseInsensitive))
        {
            token = token.mid(QString("oauth:").size()).trimmed();
        }

        if (token == previous)
        {
            break;
        }
    }

    return token;
}

QString nowIso()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

QString lower(QString text)
{
    return text.trimmed().toLower();
}

struct RefreshCoordinator {
    bool running = false;
    std::vector<std::function<void(MoltorinoAuthRefreshResult)>> callbacks;
};

RefreshCoordinator &refreshCoordinator()
{
    static RefreshCoordinator coordinator;
    return coordinator;
}

MoltorinoAuthChannel channelFromJson(const QJsonObject &obj)
{
    return {
        .id = obj.value("id").toString().trimmed(),
        .login = obj.value("login").toString().trimmed().toLower(),
        .displayName = obj.value("displayName").toString().trimmed(),
    };
}

QJsonObject channelToJson(const MoltorinoAuthChannel &channel)
{
    QJsonObject obj;
    obj.insert("id", channel.id);
    obj.insert("login", channel.login);
    obj.insert("displayName", channel.displayName);
    return obj;
}

MoltorinoAuthAccount accountFromJson(const QJsonObject &obj)
{
    MoltorinoAuthAccount account;
    account.userId = obj.value("userId").toString().trimmed();
    account.login = obj.value("login").toString().trimmed().toLower();
    account.displayName = obj.value("displayName").toString().trimmed();
    account.token = obj.value("token").toString().trimmed();
    account.valid = obj.value("valid").toBool(false);
    account.lastError = obj.value("lastError").toString();
    account.lastValidatedAt = obj.value("lastValidatedAt").toString();

    const auto channels = obj.value("moderatedChannels").toArray();
    account.moderatedChannels.reserve(channels.size());
    for (const auto &channelValue : channels)
    {
        if (channelValue.isObject())
        {
            account.moderatedChannels.push_back(
                channelFromJson(channelValue.toObject()));
        }
    }
    return account;
}

QJsonObject accountToJson(const MoltorinoAuthAccount &account)
{
    QJsonObject obj;
    obj.insert("userId", account.userId);
    obj.insert("login", account.login);
    obj.insert("displayName", account.displayName);
    obj.insert("token", account.token);
    obj.insert("valid", account.valid);
    obj.insert("lastError", account.lastError);
    obj.insert("lastValidatedAt", account.lastValidatedAt);

    QJsonArray channels;
    for (const auto &channel : account.moderatedChannels)
    {
        channels.append(channelToJson(channel));
    }
    obj.insert("moderatedChannels", channels);
    return obj;
}

void sortAccounts(std::vector<MoltorinoAuthAccount> &accounts)
{
    std::sort(accounts.begin(), accounts.end(),
              [](const auto &a, const auto &b) {
                  return lower(a.login).localeAwareCompare(lower(b.login)) < 0;
              });
}

void saveAccounts(const std::vector<MoltorinoAuthAccount> &accounts)
{
    QJsonArray array;
    for (const auto &account : accounts)
    {
        array.append(accountToJson(account));
    }

    auto json =
        QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
    getSettings()->moltorinoAuthAccounts = json;
    getSettings()->requestSave();
}

bool sameAccount(const MoltorinoAuthAccount &account, const QString &userId,
                 const QString &token)
{
    if (!userId.isEmpty() && account.userId == userId)
    {
        return true;
    }
    return !token.isEmpty() && normalizeToken(account.token) == token;
}

void upsertAccount(MoltorinoAuthAccount account)
{
    account.token = normalizeToken(account.token);
    account.login = account.login.trimmed().toLower();

    auto current = accounts();
    const auto token = account.token;
    current.erase(std::remove_if(current.begin(), current.end(),
                                 [&](const auto &existing) {
                                     return sameAccount(existing,
                                                        account.userId, token);
                                 }),
                  current.end());
    current.push_back(std::move(account));
    sortAccounts(current);
    saveAccounts(current);
}

bool accountMatchesChannel(const MoltorinoAuthAccount &account,
                           const QString &channelId,
                           const QString &channelLogin)
{
    const auto normalizedLogin = lower(channelLogin);
    if (!channelId.isEmpty() && account.userId == channelId)
    {
        return true;
    }
    if (!normalizedLogin.isEmpty() && lower(account.login) == normalizedLogin)
    {
        return true;
    }

    for (const auto &channel : account.moderatedChannels)
    {
        if (!channelId.isEmpty() && channel.id == channelId)
        {
            return true;
        }
        if (!normalizedLogin.isEmpty() &&
            lower(channel.login) == normalizedLogin)
        {
            return true;
        }
    }
    return false;
}

bool accountMatchesBroadcaster(const MoltorinoAuthAccount &account,
                               const QString &channelId,
                               const QString &channelLogin)
{
    const auto normalizedLogin = lower(channelLogin);
    if (!channelId.isEmpty() && account.userId == channelId)
    {
        return true;
    }
    return !normalizedLogin.isEmpty() &&
           lower(account.login) == normalizedLogin;
}

bool accountMatchesCurrentUser(const MoltorinoAuthAccount &account)
{
    auto current = getApp()->getAccounts()->twitch.getCurrent();
    if (!current || current->isAnon())
    {
        return false;
    }

    const auto currentUserId = current->getUserId();
    if (!currentUserId.isEmpty() && account.userId == currentUserId)
    {
        return true;
    }

    const auto currentLogin = lower(current->getUserName());
    return !currentLogin.isEmpty() && lower(account.login) == currentLogin;
}

std::vector<MoltorinoAuthAccount> validAccounts()
{
    auto loaded = accounts();
    loaded.erase(std::remove_if(loaded.begin(), loaded.end(),
                                [](const auto &account) {
                                    return !account.valid ||
                                           account.token.trimmed().isEmpty();
                                }),
                 loaded.end());
    return loaded;
}

MoltorinoAuthToken makeToken(const MoltorinoAuthAccount &account)
{
    return {
        .token = account.token,
        .userId = account.userId,
        .login = account.login,
        .legacy = false,
    };
}

MoltorinoAuthToken makeLegacyToken()
{
    auto token = MoltorinoAuthToken{
        .token = legacyToken(),
        .legacy = true,
    };

    const auto normalizedToken = normalizeToken(token.token);
    if (normalizedToken.isEmpty())
    {
        return token;
    }

    for (const auto &account : accounts())
    {
        if (normalizeToken(account.token) == normalizedToken)
        {
            token.userId = account.userId;
            token.login = account.login;
            break;
        }
    }

    return token;
}

bool hasStoredAccountForToken(const QString &token)
{
    const auto normalizedToken = normalizeToken(token);
    if (normalizedToken.isEmpty())
    {
        return false;
    }

    for (const auto &account : accounts())
    {
        if (normalizeToken(account.token) == normalizedToken)
        {
            return true;
        }
    }
    return false;
}

QString &lastResolvedPersonalToken()
{
    static QString token;
    return token;
}

MoltorinoAuthToken rememberPersonalToken(MoltorinoAuthToken token)
{
    const auto normalizedToken = normalizeToken(token.token);
    if (!normalizedToken.isEmpty())
    {
        lastResolvedPersonalToken() = normalizedToken;
    }
    return token;
}

std::optional<MoltorinoAuthAccount> validAccountForToken(
    const std::vector<MoltorinoAuthAccount> &valid, const QString &token)
{
    const auto normalizedToken = normalizeToken(token);
    if (normalizedToken.isEmpty())
    {
        return std::nullopt;
    }

    auto found =
        std::find_if(valid.begin(), valid.end(), [&](const auto &account) {
            return normalizeToken(account.token) == normalizedToken;
        });
    if (found == valid.end())
    {
        return std::nullopt;
    }
    return *found;
}

std::optional<MoltorinoAuthAccount> mostRecentlyValidatedAccount(
    const std::vector<MoltorinoAuthAccount> &valid)
{
    if (valid.empty())
    {
        return std::nullopt;
    }

    const MoltorinoAuthAccount *best = &valid.front();
    auto bestTime = QDateTime::fromString(best->lastValidatedAt, Qt::ISODate);
    for (const auto &account : valid)
    {
        const auto accountTime =
            QDateTime::fromString(account.lastValidatedAt, Qt::ISODate);
        if (!bestTime.isValid() ||
            (accountTime.isValid() && accountTime > bestTime))
        {
            best = &account;
            bestTime = accountTime;
        }
    }

    return *best;
}

std::optional<MoltorinoAuthAccount> broadcasterAccountForChannel(
    const std::vector<MoltorinoAuthAccount> &valid, const QString &channelId,
    const QString &channelLogin)
{
    for (const auto &account : valid)
    {
        if (accountMatchesCurrentUser(account) &&
            accountMatchesBroadcaster(account, channelId, channelLogin))
        {
            return account;
        }
    }

    for (const auto &account : valid)
    {
        if (accountMatchesBroadcaster(account, channelId, channelLogin))
        {
            return account;
        }
    }

    return std::nullopt;
}

bool looksLikeAuthError(const QString &error)
{
    const auto lowered = error.toLower();
    return lowered.contains("unauthenticated") ||
           lowered.contains("unauthorized") ||
           lowered.contains("authorization") ||
           lowered.contains("access token") || lowered.contains("token") ||
           lowered.contains("forbidden") || lowered.contains("401") ||
           lowered.contains("403");
}

std::shared_ptr<TwitchAccount> localAccountForAuthAccount(
    const MoltorinoAuthAccount &account)
{
    const auto normalizedLogin = lower(account.login);
    auto localAccounts = getApp()->getAccounts()->twitch.accounts.readOnly();
    for (const auto &localAccount : *localAccounts)
    {
        if (!localAccount || localAccount->isAnon() ||
            localAccount->getOAuthClient().trimmed().isEmpty() ||
            localAccount->getOAuthToken().trimmed().isEmpty())
        {
            continue;
        }

        const auto localUserId = localAccount->getUserId().trimmed();
        if (!localUserId.isEmpty() && localUserId == account.userId)
        {
            return localAccount;
        }

        const auto localLogin = lower(localAccount->getUserName());
        if (!normalizedLogin.isEmpty() && localLogin == normalizedLogin)
        {
            return localAccount;
        }
    }

    return nullptr;
}

QString moderatedChannelKey(const QString &id, const QString &login)
{
    if (!id.trimmed().isEmpty())
    {
        return QStringLiteral("id:") + id.trimmed();
    }
    if (!login.trimmed().isEmpty())
    {
        return QStringLiteral("login:") + lower(login);
    }
    return {};
}

void fetchModeratedChannelsWithHelix(
    const MoltorinoAuthAccount &account, QString clientId, QString oauthToken,
    std::function<void(QVector<MoltorinoAuthChannel>)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    clientId = clientId.trimmed();
    oauthToken = normalizeToken(oauthToken);
    if (account.userId.isEmpty() || clientId.isEmpty() || oauthToken.isEmpty())
    {
        failureCallback("Missing account details for Helix mod access");
        return;
    }

    struct FetchState {
        MoltorinoAuthAccount account;
        QString clientId;
        QString oauthToken;
        QVector<MoltorinoAuthChannel> channels;
        QSet<QString> seenChannels;
        QSet<QString> seenCursors;
        int pageCount = 0;
        bool completed = false;
        std::shared_ptr<std::function<void(QString)>> requestPage;
        std::function<void(QVector<MoltorinoAuthChannel>)> successCallback;
        std::function<void(const QString &)> failureCallback;
    };

    auto state = std::make_shared<FetchState>();
    state->account = account;
    state->clientId = std::move(clientId);
    state->oauthToken = std::move(oauthToken);
    state->successCallback = std::move(successCallback);
    state->failureCallback = std::move(failureCallback);

    auto finishSuccess = [](const std::shared_ptr<FetchState> &state) {
        if (state->completed || !state->successCallback)
        {
            return;
        }

        state->completed = true;
        auto callback = std::move(state->successCallback);
        callback(std::move(state->channels));
    };

    auto finishFailure = [](const std::shared_ptr<FetchState> &state,
                            const QString &error) {
        if (state->completed || !state->failureCallback)
        {
            return;
        }

        state->completed = true;
        auto callback = std::move(state->failureCallback);
        callback(error);
    };

    auto requestPage = std::make_shared<std::function<void(QString)>>();
    state->requestPage = requestPage;
    std::weak_ptr<FetchState> weakState = state;
    std::weak_ptr<std::function<void(QString)>> weakRequestPage = requestPage;
    *requestPage = [weakState, weakRequestPage, finishSuccess,
                    finishFailure](QString cursor) mutable {
        auto state = weakState.lock();
        if (!state)
        {
            return;
        }
        if (state->completed)
        {
            return;
        }

        if (++state->pageCount > MAX_HELIX_MODERATED_CHANNEL_PAGES)
        {
            finishFailure(state,
                          "Helix moderated channel list has too many pages");
            return;
        }

        QUrl url("https://api.twitch.tv/helix/moderation/channels");
        QUrlQuery query;
        query.addQueryItem("user_id", state->account.userId);
        query.addQueryItem("first", "100");
        if (!cursor.isEmpty())
        {
            query.addQueryItem("after", cursor);
        }
        url.setQuery(query);

        NetworkRequest(url, NetworkRequestType::Get)
            .timeout(HELIX_MODERATED_CHANNEL_TIMEOUT_MS)
            .hideRequestBody()
            .followRedirects(true)
            .header("Accept", "application/json")
            .header("Client-ID", state->clientId)
            .header("Authorization", "Bearer " + state->oauthToken)
            .onSuccess([state, weakRequestPage, finishSuccess,
                        finishFailure](const NetworkResult &result) mutable {
                const auto root = result.parseJson();
                if (!root.contains("data") || !root.value("data").isArray())
                {
                    const auto message = root.value("message").toString();
                    finishFailure(state,
                                  message.isEmpty()
                                      ? QString("Failed to parse Helix "
                                                "moderated channels response")
                                      : message);
                    return;
                }

                const auto data = root.value("data").toArray();
                if (!root.value("message").toString().isEmpty() &&
                    data.isEmpty())
                {
                    finishFailure(state, root.value("message").toString());
                    return;
                }

                for (const auto &value : data)
                {
                    const auto obj = value.toObject();
                    MoltorinoAuthChannel channel{
                        .id = obj.value("broadcaster_id").toString().trimmed(),
                        .login = obj.value("broadcaster_login")
                                     .toString()
                                     .trimmed()
                                     .toLower(),
                        .displayName =
                            obj.value("broadcaster_name").toString().trimmed(),
                    };
                    if (channel.id.isEmpty() && channel.login.isEmpty())
                    {
                        continue;
                    }

                    const auto key =
                        moderatedChannelKey(channel.id, channel.login);
                    if (key.isEmpty() || state->seenChannels.contains(key))
                    {
                        continue;
                    }

                    state->seenChannels.insert(key);
                    state->channels.push_back(std::move(channel));
                }

                const auto nextCursor = root.value("pagination")
                                            .toObject()
                                            .value("cursor")
                                            .toString();
                if (nextCursor.isEmpty())
                {
                    finishSuccess(state);
                    return;
                }

                if (state->seenCursors.contains(nextCursor))
                {
                    finishFailure(state,
                                  "Twitch repeated a Helix pagination cursor");
                    return;
                }

                state->seenCursors.insert(nextCursor);
                if (auto requestPage = weakRequestPage.lock())
                {
                    (*requestPage)(nextCursor);
                }
            })
            .onError([state,
                      finishFailure](const NetworkResult &result) mutable {
                const auto body = QString::fromUtf8(result.getData()).trimmed();
                if (!body.isEmpty())
                {
                    finishFailure(state,
                                  QString("%1 | %2").arg(result.formatError(),
                                                         body.left(200)));
                    return;
                }
                finishFailure(state, result.formatError());
            })
            .execute();
    };

    (*requestPage)({});
}

void fetchModeratedChannels(MoltorinoAuthAccount account,
                            std::function<void(MoltorinoAuthAccount)> callback)
{
    struct CallbackState {
        std::function<void(MoltorinoAuthAccount)> callback;
        bool completed = false;
    };

    auto state = std::make_shared<CallbackState>();
    state->callback = std::move(callback);

    auto finish = [state](MoltorinoAuthAccount refreshedAccount) mutable {
        if (state->completed || !state->callback)
        {
            return;
        }

        state->completed = true;
        auto callback = std::move(state->callback);
        callback(std::move(refreshedAccount));
    };

    auto finishWithChannels =
        [finish](MoltorinoAuthAccount baseAccount,
                 QVector<MoltorinoAuthChannel> channels) mutable {
            baseAccount.moderatedChannels = std::move(channels);
            baseAccount.valid = true;
            baseAccount.lastError.clear();
            baseAccount.lastValidatedAt = nowIso();
            finish(std::move(baseAccount));
        };

    auto finishWithCachedChannels = [finish](MoltorinoAuthAccount baseAccount,
                                             const QString &error) mutable {
        auto account = baseAccount;
        for (const auto &existing : accounts())
        {
            if (sameAccount(existing, account.userId,
                            normalizeToken(account.token)))
            {
                account.moderatedChannels = existing.moderatedChannels;
                break;
            }
        }
        account.valid = true;
        account.lastError =
            QString("Could not refresh mod access: %1").arg(error);
        account.lastValidatedAt = nowIso();
        finish(std::move(account));
    };

    auto fetchWithGql = [finishWithChannels, finishWithCachedChannels](
                            MoltorinoAuthAccount baseAccount) mutable {
        TwitchGql::getModeratedChannels(
            baseAccount.token,
            [baseAccount, finishWithChannels](
                QVector<GqlModeratedChannel> channels) mutable {
                QVector<MoltorinoAuthChannel> converted;
                converted.reserve(channels.size());
                for (const auto &channel : channels)
                {
                    converted.push_back({
                        .id = channel.id,
                        .login = channel.login.trimmed().toLower(),
                        .displayName = channel.displayName,
                    });
                }
                finishWithChannels(baseAccount, std::move(converted));
            },
            [baseAccount,
             finishWithCachedChannels](const QString &error) mutable {
                finishWithCachedChannels(baseAccount, error);
            });
    };

    auto baseAccount = std::move(account);
    auto localAccount = localAccountForAuthAccount(baseAccount);
    if (!localAccount)
    {
        fetchModeratedChannelsWithHelix(
            baseAccount, TWITCH_TV_CLIENT_ID, baseAccount.token,
            [baseAccount, finishWithChannels](
                QVector<MoltorinoAuthChannel> channels) mutable {
                finishWithChannels(baseAccount, std::move(channels));
            },
            [baseAccount, fetchWithGql](const QString &) mutable {
                fetchWithGql(std::move(baseAccount));
            });
        return;
    }

    fetchModeratedChannelsWithHelix(
        baseAccount, localAccount->getOAuthClient(),
        localAccount->getOAuthToken(),
        [baseAccount,
         finishWithChannels](QVector<MoltorinoAuthChannel> channels) mutable {
            finishWithChannels(baseAccount, std::move(channels));
        },
        [baseAccount, fetchWithGql,
         finishWithChannels](const QString &) mutable {
            fetchModeratedChannelsWithHelix(
                baseAccount, TWITCH_TV_CLIENT_ID, baseAccount.token,
                [baseAccount, finishWithChannels](
                    QVector<MoltorinoAuthChannel> channels) mutable {
                    finishWithChannels(baseAccount, std::move(channels));
                },
                [baseAccount, fetchWithGql](const QString &) mutable {
                    fetchWithGql(std::move(baseAccount));
                });
        });
}

void validateWithOAuth(
    const QString &token,
    std::function<void(MoltorinoAuthAccount)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    const auto normalizedToken = normalizeToken(token);
    if (normalizedToken.isEmpty())
    {
        failureCallback("No token provided");
        return;
    }

    NetworkRequest(QUrl("https://id.twitch.tv/oauth2/validate"),
                   NetworkRequestType::Get)
        .timeout(20000)
        .hideRequestBody()
        .followRedirects(true)
        .header("Accept", "application/json")
        .header("Authorization", "OAuth " + normalizedToken)
        .onSuccess([normalizedToken,
                    successCallback = std::move(successCallback),
                    failureCallback](const NetworkResult &result) {
            const auto json = result.parseJson();
            MoltorinoAuthAccount account;
            account.token = normalizedToken;
            account.userId = json.value("user_id").toString().trimmed();
            account.login = json.value("login").toString().trimmed().toLower();
            account.displayName = account.login;
            account.valid = true;
            account.lastValidatedAt = nowIso();

            if (account.userId.isEmpty() || account.login.isEmpty())
            {
                failureCallback("Twitch validated the token without returning "
                                "account details");
                return;
            }
            successCallback(std::move(account));
        })
        .onError([failureCallback =
                      std::move(failureCallback)](const NetworkResult &result) {
            const auto body = QString::fromUtf8(result.getData()).trimmed();
            if (!body.isEmpty())
            {
                failureCallback(QString("%1 | %2").arg(result.formatError(),
                                                       body.left(200)));
                return;
            }
            failureCallback(result.formatError());
        })
        .execute();
}

void validateToken(const QString &token,
                   std::function<void(MoltorinoAuthAccount)> successCallback,
                   std::function<void(const QString &)> failureCallback)
{
    struct ValidationState {
        std::function<void(MoltorinoAuthAccount)> successCallback;
        std::function<void(const QString &)> failureCallback;
        bool completed = false;
    };

    auto state = std::make_shared<ValidationState>();
    state->successCallback = std::move(successCallback);
    state->failureCallback = std::move(failureCallback);

    auto succeed = [state](MoltorinoAuthAccount account) mutable {
        if (state->completed || !state->successCallback)
        {
            return;
        }

        state->completed = true;
        auto callback = std::move(state->successCallback);
        callback(std::move(account));
    };

    auto fail = [state](const QString &error) mutable {
        if (state->completed || !state->failureCallback)
        {
            return;
        }

        state->completed = true;
        auto callback = std::move(state->failureCallback);
        callback(error);
    };

    TwitchGql::validateCustomAuthToken(
        token,
        [succeed](CustomAuthValidationResult result) mutable {
            MoltorinoAuthAccount account;
            account.token = result.normalizedToken;
            account.userId = result.userId;
            account.login = result.login.trimmed().toLower();
            account.displayName = result.displayName;
            account.valid = true;
            account.lastValidatedAt = nowIso();
            succeed(std::move(account));
        },
        [token, succeed, fail](const QString &) mutable {
            validateWithOAuth(token, std::move(succeed), std::move(fail));
        });
}

}  // namespace

std::vector<MoltorinoAuthAccount> accounts()
{
    const auto raw = getSettings()->moltorinoAuthAccounts.getValue().trimmed();
    if (raw.isEmpty())
    {
        return {};
    }

    const auto doc = QJsonDocument::fromJson(raw.toUtf8());
    if (!doc.isArray())
    {
        return {};
    }

    std::vector<MoltorinoAuthAccount> result;
    const auto array = doc.array();
    result.reserve(size_t(array.size()));
    for (const auto &value : array)
    {
        if (value.isObject())
        {
            auto account = accountFromJson(value.toObject());
            if (!account.token.trimmed().isEmpty())
            {
                result.push_back(std::move(account));
            }
        }
    }
    sortAccounts(result);
    return result;
}

MoltorinoAuthSummary summary()
{
    MoltorinoAuthSummary result;
    result.hasLegacyToken = !legacyToken().isEmpty();

    QSet<QString> channels;
    auto addChannelKey = [&channels](const QString &id, const QString &login) {
        QString key;
        if (!id.trimmed().isEmpty())
        {
            key = "id:" + id.trimmed();
        }
        else if (!login.trimmed().isEmpty())
        {
            key = "login:" + lower(login);
        }
        if (!key.isEmpty())
        {
            channels.insert(key);
        }
    };

    for (const auto &account : accounts())
    {
        ++result.accountCount;
        if (account.valid)
        {
            ++result.validAccountCount;
            addChannelKey(account.userId, account.login);
            for (const auto &channel : account.moderatedChannels)
            {
                addChannelKey(channel.id, channel.login);
            }
        }
        else
        {
            ++result.invalidAccountCount;
        }
    }

    result.moderatedChannelCount = channels.size();
    result.hasOnlyLegacyToken =
        result.accountCount == 0 && result.hasLegacyToken;
    return result;
}

QString legacyToken()
{
    return normalizeToken(getSettings()->customPinAuthToken.getValue());
}

void addOrUpdateToken(const QString &token,
                      std::function<void(MoltorinoAuthAccount)> successCallback,
                      std::function<void(const QString &)> failureCallback)
{
    validateToken(
        token,
        [successCallback =
             std::move(successCallback)](MoltorinoAuthAccount account) mutable {
            fetchModeratedChannels(
                std::move(account),
                [successCallback = std::move(successCallback)](
                    MoltorinoAuthAccount account) mutable {
                    upsertAccount(account);
                    successCallback(std::move(account));
                });
        },
        std::move(failureCallback));
}

void removeAccount(const QString &userId, const QString &token)
{
    const auto normalizedToken = normalizeToken(token);
    auto current = accounts();
    current.erase(std::remove_if(current.begin(), current.end(),
                                 [&](const auto &account) {
                                     return sameAccount(account, userId,
                                                        normalizedToken);
                                 }),
                  current.end());
    saveAccounts(current);

    if (!normalizedToken.isEmpty() && legacyToken() == normalizedToken)
    {
        getSettings()->customPinAuthToken = "";
        getSettings()->requestSave();
    }
}

void refreshAccounts(std::function<void(MoltorinoAuthRefreshResult)> callback)
{
    auto &coordinator = refreshCoordinator();
    coordinator.callbacks.push_back(std::move(callback));
    if (coordinator.running)
    {
        return;
    }
    coordinator.running = true;

    auto finishRefresh = [](MoltorinoAuthRefreshResult result) {
        auto &coordinator = refreshCoordinator();
        auto callbacks = std::move(coordinator.callbacks);
        coordinator.callbacks.clear();
        coordinator.running = false;

        for (auto &callback : callbacks)
        {
            if (callback)
            {
                callback(result);
            }
        }
    };

    auto existing = accounts();

    std::vector<QString> tokens;
    tokens.reserve(existing.size() + 1);
    for (const auto &account : existing)
    {
        const auto token = normalizeToken(account.token);
        if (!token.isEmpty() &&
            std::find(tokens.begin(), tokens.end(), token) == tokens.end())
        {
            tokens.push_back(token);
        }
    }

    const auto legacy = legacyToken();
    if (!legacy.isEmpty() &&
        std::find(tokens.begin(), tokens.end(), legacy) == tokens.end())
    {
        tokens.push_back(legacy);
    }

    if (tokens.empty())
    {
        saveAccounts({});
        finishRefresh({});
        return;
    }

    struct RefreshState {
        int pending = 0;
        std::vector<MoltorinoAuthAccount> accounts;
        MoltorinoAuthRefreshResult result;
        std::function<void(MoltorinoAuthRefreshResult)> callback;
    };

    auto state = std::make_shared<RefreshState>();
    state->pending = static_cast<int>(tokens.size());
    state->result.total = state->pending;
    state->callback = std::move(finishRefresh);

    auto existingAccountForToken = [existing](const QString &token) {
        const auto normalizedToken = normalizeToken(token);
        auto found = std::find_if(
            existing.begin(), existing.end(), [&](const auto &account) {
                return normalizeToken(account.token) == normalizedToken;
            });
        if (found != existing.end())
        {
            return *found;
        }

        MoltorinoAuthAccount account;
        account.token = normalizedToken;
        account.login = "legacy token";
        return account;
    };

    auto finishOne = [state](MoltorinoAuthAccount account) mutable {
        if (account.valid)
        {
            ++state->result.valid;
        }
        else
        {
            ++state->result.invalid;
        }
        if (!account.lastError.isEmpty())
        {
            state->result.errors.push_back(QString("%1: %2").arg(
                account.login.isEmpty() ? QString("Legacy token")
                                        : account.login,
                account.lastError));
        }

        state->accounts.push_back(std::move(account));
        --state->pending;
        if (state->pending > 0)
        {
            return;
        }

        const auto currentAccounts = accounts();

        std::vector<QString> configuredTokens;
        for (const auto &current : currentAccounts)
        {
            const auto token = normalizeToken(current.token);
            if (!token.isEmpty())
            {
                configuredTokens.push_back(token);
            }
        }

        const auto legacy = legacyToken();
        if (!legacy.isEmpty())
        {
            configuredTokens.push_back(legacy);
        }

        state->accounts.erase(
            std::remove_if(
                state->accounts.begin(), state->accounts.end(),
                [&](const auto &refreshed) {
                    const auto token = normalizeToken(refreshed.token);
                    return token.isEmpty() ||
                           std::find(configuredTokens.begin(),
                                     configuredTokens.end(),
                                     token) == configuredTokens.end();
                }),
            state->accounts.end());

        std::vector<QString> refreshedTokens;
        refreshedTokens.reserve(state->accounts.size());
        for (const auto &refreshed : state->accounts)
        {
            const auto token = normalizeToken(refreshed.token);
            if (!token.isEmpty())
            {
                refreshedTokens.push_back(token);
            }
        }

        for (const auto &current : currentAccounts)
        {
            const auto token = normalizeToken(current.token);
            if (token.isEmpty())
            {
                continue;
            }
            if (std::find(configuredTokens.begin(), configuredTokens.end(),
                          token) == configuredTokens.end())
            {
                continue;
            }
            if (std::find(refreshedTokens.begin(), refreshedTokens.end(),
                          token) != refreshedTokens.end())
            {
                continue;
            }

            state->accounts.push_back(current);
            refreshedTokens.push_back(token);
        }

        sortAccounts(state->accounts);
        saveAccounts(state->accounts);
        state->result.moderatedChannels = summary().moderatedChannelCount;
        state->callback(state->result);
    };

    for (const auto &token : tokens)
    {
        validateToken(
            token,
            [finishOne](MoltorinoAuthAccount account) mutable {
                fetchModeratedChannels(std::move(account), finishOne);
            },
            [token, finishOne,
             existingAccountForToken](const QString &error) mutable {
                auto account = existingAccountForToken(token);
                account.valid = account.valid && !looksLikeAuthError(error);
                account.lastError = error;
                account.lastValidatedAt = nowIso();
                finishOne(std::move(account));
            });
    }
}

void scheduleStartupRefresh()
{
    static bool scheduled = false;
    if (scheduled)
    {
        return;
    }
    scheduled = true;

    const auto currentSummary = summary();
    if (currentSummary.accountCount == 0 && !currentSummary.hasLegacyToken)
    {
        return;
    }

    auto *app = QCoreApplication::instance();
    if (app == nullptr)
    {
        return;
    }

    QTimer::singleShot(10000, app, [] {
        const auto currentSummary = summary();
        if (currentSummary.accountCount == 0 && !currentSummary.hasLegacyToken)
        {
            return;
        }

        refreshAccounts([](MoltorinoAuthRefreshResult) {});
    });
}

MoltorinoAuthToken resolveModerationToken(const QString &channelId,
                                          const QString &channelLogin,
                                          QString *errorMessage)
{
    const auto valid = validAccounts();
    if (!valid.empty())
    {
        for (const auto &account : valid)
        {
            if (accountMatchesCurrentUser(account) &&
                accountMatchesChannel(account, channelId, channelLogin))
            {
                return makeToken(account);
            }
        }

        for (const auto &account : valid)
        {
            if (accountMatchesChannel(account, channelId, channelLogin))
            {
                return makeToken(account);
            }
        }

        const auto legacy = legacyToken();
        if (!legacy.isEmpty() && !hasStoredAccountForToken(legacy))
        {
            return makeLegacyToken();
        }

        if (errorMessage)
        {
            *errorMessage =
                QString("No saved account has cached moderator access "
                        "for #%1. Refresh accounts in Settings -> Moltorino -> "
                        "Authentication or add the account that moderates this "
                        "channel.")
                    .arg(channelLogin);
        }
        return {};
    }

    if (!legacyToken().isEmpty())
    {
        return makeLegacyToken();
    }

    if (errorMessage)
    {
        *errorMessage = authRequiredMessage("this action");
    }
    return {};
}

MoltorinoAuthToken resolveSavedBroadcasterToken(const QString &channelId,
                                                const QString &channelLogin,
                                                QString *errorMessage)
{
    const auto valid = validAccounts();
    if (const auto account =
            broadcasterAccountForChannel(valid, channelId, channelLogin))
    {
        return makeToken(*account);
    }

    if (errorMessage)
    {
        *errorMessage =
            QString("No saved broadcaster account matches #%1. Add the "
                    "broadcaster account in Settings -> Moltorino -> "
                    "Authentication.")
                .arg(channelLogin);
    }
    return {};
}

MoltorinoAuthToken resolveBroadcasterToken(const QString &channelId,
                                           const QString &channelLogin,
                                           QString *errorMessage)
{
    const auto valid = validAccounts();
    if (!valid.empty())
    {
        if (const auto account =
                broadcasterAccountForChannel(valid, channelId, channelLogin))
        {
            return makeToken(*account);
        }

        const auto legacy = legacyToken();
        if (!legacy.isEmpty() && !hasStoredAccountForToken(legacy))
        {
            return makeLegacyToken();
        }

        if (errorMessage)
        {
            *errorMessage =
                QString("No saved account matches #%1. Add the broadcaster "
                        "account in Settings -> Moltorino -> Authentication.")
                    .arg(channelLogin);
        }
        return {};
    }

    if (!legacyToken().isEmpty())
    {
        return makeLegacyToken();
    }

    if (errorMessage)
    {
        *errorMessage = authRequiredMessage("raid controls");
    }
    return {};
}

MoltorinoAuthToken resolveSelectedUserToken(QString *errorMessage)
{
    const auto valid = validAccounts();
    for (const auto &account : valid)
    {
        if (accountMatchesCurrentUser(account))
        {
            return rememberPersonalToken(makeToken(account));
        }
    }

    if (errorMessage)
    {
        *errorMessage =
            "Saved login for the current Twitch account was not found. "
            "Add this account in Settings -> Moltorino -> Authentication.";
    }
    return {};
}

MoltorinoAuthToken resolveCurrentUserToken(QString *errorMessage)
{
    auto current = getApp()->getAccounts()->twitch.getCurrent();
    const auto currentUserId =
        current && !current->isAnon() ? current->getUserId() : QString();
    const auto currentLogin = current && !current->isAnon()
                                  ? lower(current->getUserName())
                                  : QString();

    const auto valid = validAccounts();
    if (!valid.empty())
    {
        for (const auto &account : valid)
        {
            if ((!currentUserId.isEmpty() && account.userId == currentUserId) ||
                (!currentLogin.isEmpty() &&
                 lower(account.login) == currentLogin))
            {
                return rememberPersonalToken(makeToken(account));
            }
        }

        if (!legacyToken().isEmpty())
        {
            return rememberPersonalToken(makeLegacyToken());
        }

        if (auto lastAccount =
                validAccountForToken(valid, lastResolvedPersonalToken()))
        {
            return rememberPersonalToken(makeToken(*lastAccount));
        }

        if (valid.size() == 1)
        {
            return rememberPersonalToken(makeToken(valid.front()));
        }

        if (auto latest = mostRecentlyValidatedAccount(valid))
        {
            return rememberPersonalToken(makeToken(*latest));
        }

        if (errorMessage)
        {
            *errorMessage =
                "Saved login for the current Twitch account was not found. "
                "Add this account in Settings -> Moltorino -> Authentication.";
        }
        return {};
    }

    if (!legacyToken().isEmpty())
    {
        return rememberPersonalToken(makeLegacyToken());
    }

    if (errorMessage)
    {
        *errorMessage = authRequiredMessage("this action");
    }
    return {};
}

MoltorinoAuthToken resolveReadToken(QString *errorMessage)
{
    QString ignored;
    auto currentUserToken = resolveCurrentUserToken(&ignored);
    if (currentUserToken.hasToken())
    {
        return currentUserToken;
    }

    auto current = getApp()->getAccounts()->twitch.getCurrent();
    if (current && !current->isAnon() && !current->getOAuthToken().isEmpty())
    {
        return {
            .token = current->getOAuthToken(),
            .userId = current->getUserId(),
            .login = current->getUserName(),
            .legacy = false,
        };
    }

    if (errorMessage)
    {
        *errorMessage =
            ignored.isEmpty() ? authRequiredMessage("this action") : ignored;
    }
    return {};
}

QString authRequiredMessage(const QString &action)
{
    return QString("Additional login required for %1. Add an account in "
                   "Settings -> "
                   "Moltorino -> Authentication, then try again.")
        .arg(action);
}

QString authExpiredMessage(const QString &action)
{
    return QString("Your saved login is missing or may have expired. "
                   "Refresh accounts or add the account again in Settings -> "
                   "Moltorino -> Authentication, then try %1 again.")
        .arg(action);
}

QString normalizeAuthError(const QString &action, const QString &error)
{
    if (looksLikeAuthError(error))
    {
        return authExpiredMessage(action);
    }
    return error;
}

}  // namespace chatterino::MoltorinoAuth

// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/api/Helix.hpp"

#include "Application.hpp"
#include "common/Args.hpp"
#include "common/Literals.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "util/CancellationToken.hpp"
#include "util/QMagicEnum.hpp"

#include <magic_enum/magic_enum.hpp>
#include <QJsonDocument>
#include <QStringBuilder>

namespace {

using namespace chatterino;

constexpr auto NUM_MODERATORS_TO_FETCH_PER_REQUEST = 100;

constexpr auto NUM_CHATTERS_TO_FETCH = 1000;

}  // namespace

namespace chatterino {

using namespace literals;

static IHelix *instance = nullptr;

HelixChatters::HelixChatters(const QJsonObject &jsonObject)
    : total(jsonObject.value("total").toInt())
    , cursor(
          jsonObject.value("pagination").toObject().value("cursor").toString())
{
    const auto &data = jsonObject.value("data").toArray();
    for (const auto &chatter : data)
    {
        auto userLogin = chatter.toObject().value("user_login").toString();
        this->chatters.insert(userLogin);
    }
}

void Helix::fetchUsers(QStringList userIds, QStringList userLogins,
                       ResultCallback<std::vector<HelixUser>> successCallback,
                       HelixFailureCallback failureCallback)
{
    QUrlQuery urlQuery;

    for (const auto &id : userIds)
    {
        urlQuery.addQueryItem("id", id);
    }

    for (const auto &login : userLogins)
    {
        urlQuery.addQueryItem("login", login);
    }

    this->makeGet("users", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();
            auto data = root.value("data");

            if (!data.isArray())
            {
                failureCallback();
                return;
            }

            std::vector<HelixUser> users;

            for (const auto &jsonUser : data.toArray())
            {
                users.emplace_back(jsonUser.toObject());
            }

            successCallback(users);
        })
        .onError([failureCallback](auto) {
            failureCallback();
        })
        .execute();
}

void Helix::getUserByName(QString userName,
                          ResultCallback<HelixUser> successCallback,
                          HelixFailureCallback failureCallback)
{
    QStringList userIds;
    QStringList userLogins{std::move(userName)};

    this->fetchUsers(
        userIds, userLogins,
        [successCallback,
         failureCallback](const std::vector<HelixUser> &users) {
            if (users.empty())
            {
                failureCallback();
                return;
            }
            successCallback(users[0]);
        },
        failureCallback);
}

void Helix::getUserById(QString userId,
                        ResultCallback<HelixUser> successCallback,
                        HelixFailureCallback failureCallback)
{
    QStringList userIds{std::move(userId)};
    QStringList userLogins;

    this->fetchUsers(
        userIds, userLogins,
        [successCallback, failureCallback](const auto &users) {
            if (users.empty())
            {
                failureCallback();
                return;
            }
            successCallback(users[0]);
        },
        failureCallback);
}

void Helix::getChannelFollowers(
    QString broadcasterID,
    ResultCallback<HelixGetChannelFollowersResponse> successCallback,
    std::function<void(QString)> failureCallback)
{
    assert(!broadcasterID.isEmpty());

    QUrlQuery urlQuery;
    urlQuery.addQueryItem("broadcaster_id", broadcasterID);

    this->makeGet("channels/followers", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();
            if (root.empty())
            {
                failureCallback("Bad JSON response");
                return;
            }
            successCallback(HelixGetChannelFollowersResponse(root));
        })
        .onError([failureCallback](auto result) {
            auto root = result.parseJson();
            if (root.empty())
            {
                failureCallback("Unknown error");
                return;
            }

            HelixError error(root);
            failureCallback(error.message);
        })
        .execute();
}

void Helix::fetchStreams(
    QStringList userIds, QStringList userLogins,
    ResultCallback<std::vector<HelixStream>> successCallback,
    HelixFailureCallback failureCallback, std::function<void()> finallyCallback)
{
    QUrlQuery urlQuery;

    for (const auto &id : userIds)
    {
        urlQuery.addQueryItem("user_id", id);
    }

    for (const auto &login : userLogins)
    {
        urlQuery.addQueryItem("user_login", login);
    }

    this->makeGet("streams", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();
            auto data = root.value("data");

            if (!data.isArray())
            {
                failureCallback();
                return;
            }

            std::vector<HelixStream> streams;

            for (const auto &jsonStream : data.toArray())
            {
                streams.emplace_back(jsonStream.toObject());
            }

            successCallback(streams);
        })
        .onError([failureCallback](auto) {
            failureCallback();
        })
        .finally(finallyCallback)
        .execute();
}

void Helix::getStreamById(QString userId,
                          ResultCallback<bool, HelixStream> successCallback,
                          HelixFailureCallback failureCallback,
                          std::function<void()> finallyCallback)
{
    QStringList userIds{std::move(userId)};
    QStringList userLogins;

    this->fetchStreams(
        userIds, userLogins,
        [successCallback](const auto &streams) {
            if (streams.empty())
            {
                successCallback(false, HelixStream());
                return;
            }
            successCallback(true, streams[0]);
        },
        failureCallback, finallyCallback);
}

void Helix::getStreamByName(QString userName,
                            ResultCallback<bool, HelixStream> successCallback,
                            HelixFailureCallback failureCallback,
                            std::function<void()> finallyCallback)
{
    QStringList userIds;
    QStringList userLogins{std::move(userName)};

    this->fetchStreams(
        userIds, userLogins,
        [successCallback, failureCallback](const auto &streams) {
            if (streams.empty())
            {
                successCallback(false, HelixStream());
                return;
            }
            successCallback(true, streams[0]);
        },
        failureCallback, finallyCallback);
}

void Helix::fetchGames(QStringList gameIds, QStringList gameNames,
                       ResultCallback<std::vector<HelixGame>> successCallback,
                       HelixFailureCallback failureCallback)
{
    assert((gameIds.length() + gameNames.length()) > 0);

    QUrlQuery urlQuery;

    for (const auto &id : gameIds)
    {
        urlQuery.addQueryItem("id", id);
    }

    for (const auto &login : gameNames)
    {
        urlQuery.addQueryItem("name", login);
    }

    this->makeGet("games", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();
            auto data = root.value("data");

            if (!data.isArray())
            {
                failureCallback();
                return;
            }

            std::vector<HelixGame> games;

            for (const auto &jsonStream : data.toArray())
            {
                games.emplace_back(jsonStream.toObject());
            }

            successCallback(games);
        })
        .onError([failureCallback](auto) {
            failureCallback();
        })
        .execute();
}

void Helix::searchGames(QString gameName,
                        ResultCallback<std::vector<HelixGame>> successCallback,
                        HelixFailureCallback failureCallback)
{
    QUrlQuery urlQuery;
    urlQuery.addQueryItem("query", gameName);

    this->makeGet("search/categories", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();
            auto data = root.value("data");

            if (!data.isArray())
            {
                failureCallback();
                return;
            }

            std::vector<HelixGame> games;

            for (const auto &jsonStream : data.toArray())
            {
                games.emplace_back(jsonStream.toObject());
            }

            successCallback(games);
        })
        .onError([failureCallback](auto) {
            failureCallback();
        })
        .execute();
}

void Helix::getGameById(QString gameId,
                        ResultCallback<HelixGame> successCallback,
                        HelixFailureCallback failureCallback)
{
    QStringList gameIds{std::move(gameId)};
    QStringList gameNames;

    this->fetchGames(
        gameIds, gameNames,
        [successCallback, failureCallback](const auto &games) {
            if (games.empty())
            {
                failureCallback();
                return;
            }
            successCallback(games[0]);
        },
        failureCallback);
}

void Helix::createClip(
    QString channelId, QString title, std::optional<int> duration,
    ResultCallback<HelixClip> successCallback,
    std::function<void(HelixClipError, QString)> failureCallback,
    std::function<void()> finallyCallback)
{
    QUrlQuery urlQuery;
    urlQuery.addQueryItem("broadcaster_id", channelId);

    if (!title.isEmpty())
    {
        urlQuery.addQueryItem("title", title);
    }

    if (duration.has_value())
    {
        urlQuery.addQueryItem("duration", QString::number(*duration));
    }

    this->makePost("clips", urlQuery)
        .header("Content-Type", "application/json")
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();
            auto data = root.value("data");

            if (!data.isArray())
            {
                failureCallback(HelixClipError::Unknown, "No clip was created");
                return;
            }

            HelixClip clip(data.toArray()[0].toObject());

            successCallback(clip);
        })
        .onError([failureCallback](auto result) {
            auto obj = result.parseJson();
            auto message = obj.value("message").toString();
            switch (result.status().value_or(0))
            {
                case 503: {
                    failureCallback(HelixClipError::ClipsUnavailable, message);
                }
                break;

                case 401: {
                    failureCallback(HelixClipError::UserNotAuthenticated,
                                    message);
                }
                break;

                case 403: {
                    if (message.contains("restricted for this channel"))
                    {
                        failureCallback(HelixClipError::ClipsDisabled, message);
                    }
                    else if (message.contains("User does not have permissions"))
                    {
                        failureCallback(HelixClipError::ClipsRestricted,
                                        message);
                    }
                    else if (message.contains("restricted for this category"))
                    {
                        failureCallback(HelixClipError::ClipsRestrictedCategory,
                                        message);
                    }
                    else
                    {
                        qCDebug(chatterinoTwitch)
                            << "Failed to create a clip: "
                            << result.formatError() << result.getData();
                        failureCallback(HelixClipError::Unknown, message);
                    }
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Failed to create a clip: " << result.formatError()
                        << result.getData();
                    failureCallback(HelixClipError::Unknown, message);
                }
                break;
            }
        })
        .finally(std::move(finallyCallback))
        .execute();
}

void Helix::fetchChannels(
    QStringList userIDs,
    ResultCallback<std::vector<HelixChannel>> successCallback,
    HelixFailureCallback failureCallback)
{
    QUrlQuery urlQuery;

    for (const auto &userID : userIDs)
    {
        urlQuery.addQueryItem("broadcaster_id", userID);
    }

    this->makeGet("channels", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();
            auto data = root.value("data");

            if (!data.isArray())
            {
                failureCallback();
                return;
            }

            std::vector<HelixChannel> channels;

            for (const auto &unparsedChannel : data.toArray())
            {
                channels.emplace_back(unparsedChannel.toObject());
            }

            successCallback(channels);
        })
        .onError([failureCallback](auto) {
            failureCallback();
        })
        .execute();
}

void Helix::getChannel(QString broadcasterId,
                       ResultCallback<HelixChannel> successCallback,
                       HelixFailureCallback failureCallback)
{
    QUrlQuery urlQuery;
    urlQuery.addQueryItem("broadcaster_id", broadcasterId);

    this->makeGet("channels", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();
            auto data = root.value("data");

            if (!data.isArray())
            {
                failureCallback();
                return;
            }

            HelixChannel channel(data.toArray()[0].toObject());

            successCallback(channel);
        })
        .onError([failureCallback](auto) {
            failureCallback();
        })
        .execute();
}

void Helix::createStreamMarker(
    QString broadcasterId, QString description,
    ResultCallback<HelixStreamMarker> successCallback,
    std::function<void(HelixStreamMarkerError)> failureCallback)
{
    QJsonObject payload;

    if (!description.isEmpty())
    {
        payload.insert("description", QJsonValue(description));
    }
    payload.insert("user_id", QJsonValue(broadcasterId));

    this->makePost("streams/markers", QUrlQuery())
        .json(payload)
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();
            auto data = root.value("data");

            if (!data.isArray())
            {
                failureCallback(HelixStreamMarkerError::Unknown);
                return;
            }

            HelixStreamMarker streamMarker(data.toArray()[0].toObject());

            successCallback(streamMarker);
        })
        .onError([failureCallback](NetworkResult result) {
            switch (result.status().value_or(0))
            {
                case 403: {
                    failureCallback(HelixStreamMarkerError::UserNotAuthorized);
                }
                break;

                case 401: {
                    failureCallback(
                        HelixStreamMarkerError::UserNotAuthenticated);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Failed to create a stream marker: "
                        << result.formatError() << result.getData();
                    failureCallback(HelixStreamMarkerError::Unknown);
                }
                break;
            }
        })
        .execute();
};

void Helix::loadBlocks(QString userId,
                       ResultCallback<std::vector<HelixBlock>> pageCallback,
                       FailureCallback<QString> failureCallback,
                       CancellationToken &&token)
{
    constexpr const size_t blockLimit = 1000;

    QUrlQuery query;
    query.addQueryItem(u"broadcaster_id"_s, userId);
    query.addQueryItem(u"first"_s, u"100"_s);

    size_t receivedItems = 0;
    this->paginate(
        u"users/blocks"_s, query,
        [pageCallback, receivedItems](const QJsonObject &json,
                                      const auto &) mutable {
            const auto data = json["data"_L1].toArray();

            if (data.isEmpty())
            {
                return false;
            }

            std::vector<HelixBlock> ignores;
            ignores.reserve(data.count());

            for (const auto &ignore : data)
            {
                ignores.emplace_back(ignore.toObject());
            }

            pageCallback(ignores);

            receivedItems += data.count();

            if (receivedItems >= blockLimit)
            {
                qCInfo(chatterinoTwitch) << "Reached the limit of" << blockLimit
                                         << "Twitch blocks fetched";
                return false;
            }

            return true;
        },
        [failureCallback](const NetworkResult &result) {
            failureCallback(result.formatError());
        },
        std::move(token));
}

void Helix::blockUser(QString targetUserId, const QObject *caller,
                      std::function<void()> successCallback,
                      HelixFailureCallback failureCallback)
{
    QUrlQuery urlQuery;
    urlQuery.addQueryItem("target_user_id", targetUserId);

    this->makePut("users/blocks", urlQuery)
        .caller(caller)
        .onSuccess([successCallback](auto) {
            successCallback();
        })
        .onError([failureCallback](auto) {
            failureCallback();
        })
        .execute();
}

void Helix::unblockUser(QString targetUserId, const QObject *caller,
                        std::function<void()> successCallback,
                        HelixFailureCallback failureCallback)
{
    QUrlQuery urlQuery;
    urlQuery.addQueryItem("target_user_id", targetUserId);

    this->makeDelete("users/blocks", urlQuery)
        .caller(caller)
        .onSuccess([successCallback](auto) {
            successCallback();
        })
        .onError([failureCallback](auto) {
            failureCallback();
        })
        .execute();
}

void Helix::updateChannel(
    QString broadcasterId, QString gameId, QString language, QString title,
    std::function<void(NetworkResult)> successCallback,
    FailureCallback<HelixUpdateChannelError, QString> failureCallback)
{
    using Error = HelixUpdateChannelError;

    QUrlQuery urlQuery;
    auto obj = QJsonObject();
    if (!gameId.isEmpty())
    {
        obj.insert("game_id", gameId);
    }
    if (!language.isEmpty())
    {
        obj.insert("broadcaster_language", language);
    }
    if (!title.isEmpty())
    {
        obj.insert("title", title);
    }

    if (title.isEmpty() && gameId.isEmpty() && language.isEmpty())
    {
        qCDebug(chatterinoCommon) << "Tried to update channel with no changes!";
        return;
    }

    urlQuery.addQueryItem("broadcaster_id", broadcasterId);
    this->makePatch("channels", urlQuery)
        .json(obj)
        .onSuccess([successCallback, failureCallback](auto result) {
            successCallback(result);
        })
        .onError([failureCallback](NetworkResult result) {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.compare(
                                 "The ID in broadcaster_id must match the user "
                                 "ID found in the request's OAuth token.",
                                 Qt::CaseInsensitive) == 0)
                    {
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 400:
                case 403: {
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                case 500: {
                    failureCallback(Error::Unknown, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Helix update channel, unhandled error data:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::manageAutoModMessages(
    QString userID, QString msgID, QString action,
    std::function<void()> successCallback,
    std::function<void(HelixAutoModMessageError)> failureCallback)
{
    QJsonObject payload;

    payload.insert("user_id", userID);
    payload.insert("msg_id", msgID);
    payload.insert("action", action);

    this->makePost("moderation/automod/message", QUrlQuery())
        .json(payload)
        .onSuccess([successCallback, failureCallback](auto result) {
            successCallback();
        })
        .onError([failureCallback, msgID, action](NetworkResult result) {
            switch (result.status().value_or(0))
            {
                case 400: {
                    failureCallback(
                        HelixAutoModMessageError::MessageAlreadyProcessed);
                }
                break;

                case 401: {
                    failureCallback(
                        HelixAutoModMessageError::UserNotAuthenticated);
                }
                break;

                case 403: {
                    failureCallback(
                        HelixAutoModMessageError::UserNotAuthorized);
                }
                break;

                case 404: {
                    failureCallback(HelixAutoModMessageError::MessageNotFound);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Failed to manage automod message: " << action
                        << msgID << result.formatError() << result.getData();
                    failureCallback(HelixAutoModMessageError::Unknown);
                }
                break;
            }
        })
        .execute();
}

void Helix::getCheermotes(
    QString broadcasterId,
    ResultCallback<std::vector<HelixCheermoteSet>> successCallback,
    HelixFailureCallback failureCallback)
{
    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterId);

    this->makeGet("bits/cheermotes", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();
            auto data = root.value("data");

            if (!data.isArray())
            {
                failureCallback();
                return;
            }

            std::vector<HelixCheermoteSet> cheermoteSets;

            for (const auto &jsonStream : data.toArray())
            {
                cheermoteSets.emplace_back(jsonStream.toObject());
            }

            successCallback(cheermoteSets);
        })
        .onError([broadcasterId, failureCallback](NetworkResult result) {
            qCDebug(chatterinoTwitch)
                << "Failed to get cheermotes(broadcaster_id=" << broadcasterId
                << "): " << result.formatError() << result.getData();
            failureCallback();
        })
        .execute();
}

void Helix::getEmoteSetData(QString emoteSetId,
                            ResultCallback<HelixEmoteSetData> successCallback,
                            HelixFailureCallback failureCallback)
{
    QUrlQuery urlQuery;

    urlQuery.addQueryItem("emote_set_id", emoteSetId);

    this->makeGet("chat/emotes/set", urlQuery)
        .onSuccess([successCallback, failureCallback, emoteSetId](auto result) {
            QJsonObject root = result.parseJson();
            auto data = root.value("data");

            if (!data.isArray() || data.toArray().isEmpty())
            {
                failureCallback();
                return;
            }

            HelixEmoteSetData emoteSetData(data.toArray()[0].toObject());

            successCallback(emoteSetData);
        })
        .onError([failureCallback](NetworkResult result) {
            failureCallback();
        })
        .execute();
}

void Helix::getChannelEmotes(
    QString broadcasterId,
    ResultCallback<std::vector<HelixChannelEmote>> successCallback,
    HelixFailureCallback failureCallback)
{
    QUrlQuery urlQuery;
    urlQuery.addQueryItem("broadcaster_id", broadcasterId);

    this->makeGet("chat/emotes", urlQuery)
        .onSuccess([successCallback, failureCallback](NetworkResult result) {
            QJsonObject root = result.parseJson();
            auto data = root.value("data");

            if (!data.isArray())
            {
                failureCallback();
                return;
            }

            std::vector<HelixChannelEmote> channelEmotes;

            for (const auto &jsonStream : data.toArray())
            {
                channelEmotes.emplace_back(jsonStream.toObject());
            }

            successCallback(channelEmotes);
        })
        .onError([failureCallback](auto result) {
            failureCallback();
        })
        .execute();
}

void Helix::updateUserChatColor(
    QString userID, QString color, ResultCallback<> successCallback,
    FailureCallback<HelixUpdateUserChatColorError, QString> failureCallback)
{
    using Error = HelixUpdateUserChatColorError;

    QJsonObject payload;

    payload.insert("user_id", QJsonValue(userID));
    payload.insert("color", QJsonValue(color));

    this->makePut("chat/color", QUrlQuery())
        .json(payload)
        .onSuccess([successCallback, failureCallback](auto result) {
            auto obj = result.parseJson();
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for updating chat color was"
                    << result.formatError()
                    << "but we only expected it to be 204";
            }

            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.startsWith("invalid color",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::InvalidColor, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error changing user color:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
};

void Helix::deleteChatMessages(
    QString broadcasterID, QString moderatorID, QString messageID,
    ResultCallback<> successCallback,
    FailureCallback<HelixDeleteChatMessagesError, QString> failureCallback)
{
    using Error = HelixDeleteChatMessagesError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("moderator_id", moderatorID);

    if (!messageID.isEmpty())
    {
        urlQuery.addQueryItem("message_id", messageID);
    }

    this->makeDelete("moderation/chat", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for deleting chat messages was"
                    << result.formatError()
                    << "but we only expected it to be 204";
            }

            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 404: {
                    failureCallback(Error::MessageUnavailable, message);
                }
                break;

                case 400: {
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 403: {
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error deleting chat messages:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::addChannelModerator(
    QString broadcasterID, QString userID, ResultCallback<> successCallback,
    FailureCallback<HelixAddChannelModeratorError, QString> failureCallback)
{
    using Error = HelixAddChannelModeratorError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("user_id", userID);

    this->makePost("moderation/moderators", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for adding a moderator was"
                    << result.formatError()
                    << "but we only expected it to be 204";
            }

            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.compare("incorrect user authorization",
                                             Qt::CaseInsensitive) == 0)
                    {
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 400: {
                    if (message.compare("user is already a mod",
                                        Qt::CaseInsensitive) == 0)
                    {
                        failureCallback(Error::TargetAlreadyModded, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 422: {
                    failureCallback(Error::TargetIsVIP, message);
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error adding channel moderator:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::removeChannelModerator(
    QString broadcasterID, QString userID, ResultCallback<> successCallback,
    FailureCallback<HelixRemoveChannelModeratorError, QString> failureCallback)
{
    using Error = HelixRemoveChannelModeratorError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("user_id", userID);

    this->makeDelete("moderation/moderators", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for unmodding user was"
                    << result.formatError()
                    << "but we only expected it to be 204";
            }

            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.compare("user is not a mod",
                                        Qt::CaseInsensitive) == 0)
                    {
                        failureCallback(Error::TargetNotModded, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.compare("incorrect user authorization",
                                             Qt::CaseInsensitive) == 0)
                    {
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error unmodding user:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::sendChatAnnouncement(
    QString broadcasterID, QString moderatorID, QString message,
    HelixAnnouncementColor color, ResultCallback<> successCallback,
    FailureCallback<HelixSendChatAnnouncementError, QString> failureCallback)
{
    using Error = HelixSendChatAnnouncementError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("moderator_id", moderatorID);

    QJsonObject body;
    body.insert("message", message);
    body.insert("color", qmagicenum::enumNameString(color).toLower());

    this->makePost("chat/announcements", urlQuery)
        .json(body)
        .onSuccess([successCallback, failureCallback](auto result) {
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for sending an announcement was"
                    << result.formatError()
                    << "but we only expected it to be 204";
            }

            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 403: {
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error sending an announcement:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::addChannelVIP(
    QString broadcasterID, QString userID, ResultCallback<> successCallback,
    FailureCallback<HelixAddChannelVIPError, QString> failureCallback)
{
    using Error = HelixAddChannelVIPError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("user_id", userID);

    this->makePost("channels/vips", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for adding channel VIP was"
                    << result.formatError()
                    << "but we only expected it to be 204";
            }

            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400:
                case 409:
                case 422:
                case 425: {
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.compare("incorrect user authorization",
                                             Qt::CaseInsensitive) == 0 ||
                             message.startsWith("the id in broadcaster_id must "
                                                "match the user id",
                                                Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error adding channel VIP:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::removeChannelVIP(
    QString broadcasterID, QString userID, ResultCallback<> successCallback,
    FailureCallback<HelixRemoveChannelVIPError, QString> failureCallback)
{
    using Error = HelixRemoveChannelVIPError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("user_id", userID);

    this->makeDelete("channels/vips", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for removing channel VIP was"
                    << result.formatError()
                    << "but we only expected it to be 204";
            }

            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400:
                case 409:
                case 422: {
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.compare("incorrect user authorization",
                                             Qt::CaseInsensitive) == 0 ||
                             message.startsWith("the id in broadcaster_id must "
                                                "match the user id",
                                                Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error removing channel VIP:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::unbanUser(
    QString broadcasterID, QString moderatorID, QString userID,
    ResultCallback<> successCallback,
    FailureCallback<HelixUnbanUserError, QString> failureCallback)
{
    using Error = HelixUnbanUserError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("moderator_id", moderatorID);
    urlQuery.addQueryItem("user_id", userID);

    this->makeDelete("moderation/bans", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for unbanning user was"
                    << result.formatError()
                    << "but we only expected it to be 204";
            }

            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.startsWith("The user in the user_id query "
                                           "parameter is not banned",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::TargetNotBanned, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 409: {
                    failureCallback(Error::ConflictingOperation, message);
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.compare("incorrect user authorization",
                                             Qt::CaseInsensitive) == 0 ||
                             message.startsWith("the id in broadcaster_id must "
                                                "match the user id",
                                                Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 403: {
                    failureCallback(Error::UserNotAuthorized, message);
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error unbanning user:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::startRaid(
    QString fromBroadcasterID, QString toBroadcasterID,
    ResultCallback<> successCallback,
    FailureCallback<HelixStartRaidError, QString> failureCallback)
{
    using Error = HelixStartRaidError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("from_broadcaster_id", fromBroadcasterID);
    urlQuery.addQueryItem("to_broadcaster_id", toBroadcasterID);

    this->makePost("raids", urlQuery)
        .onSuccess([successCallback, failureCallback](auto) {
            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.compare("The IDs in from_broadcaster_id and "
                                        "to_broadcaster_id cannot be the same.",
                                        Qt::CaseInsensitive) == 0)
                    {
                        failureCallback(Error::CantRaidYourself, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.compare(
                                 "The ID in broadcaster_id must match the user "
                                 "ID "
                                 "found in the request's OAuth token.",
                                 Qt::CaseInsensitive) == 0)
                    {
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 409: {
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error while starting a raid:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::cancelRaid(
    QString broadcasterID, ResultCallback<> successCallback,
    FailureCallback<HelixCancelRaidError, QString> failureCallback)
{
    using Error = HelixCancelRaidError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);

    this->makeDelete("raids", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for canceling the raid was"
                    << result.formatError()
                    << "but we only expected it to be 204";
            }

            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.compare(
                                 "The ID in broadcaster_id must match the user "
                                 "ID "
                                 "found in the request's OAuth token.",
                                 Qt::CaseInsensitive) == 0)
                    {
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 404: {
                    failureCallback(Error::NoRaidPending, message);
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error while canceling the raid:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::updateEmoteMode(
    QString broadcasterID, QString moderatorID, bool emoteMode,
    ResultCallback<HelixChatSettings> successCallback,
    FailureCallback<HelixUpdateChatSettingsError, QString> failureCallback)
{
    QJsonObject json;
    json["emote_mode"] = emoteMode;
    this->updateChatSettings(broadcasterID, moderatorID, json, successCallback,
                             failureCallback);
}

void Helix::updateFollowerMode(
    QString broadcasterID, QString moderatorID,
    std::optional<int> followerModeDuration,
    ResultCallback<HelixChatSettings> successCallback,
    FailureCallback<HelixUpdateChatSettingsError, QString> failureCallback)
{
    QJsonObject json;
    json["follower_mode"] = followerModeDuration.has_value();
    if (followerModeDuration)
    {
        json["follower_mode_duration"] = *followerModeDuration;
    }

    this->updateChatSettings(broadcasterID, moderatorID, json, successCallback,
                             failureCallback);
}

void Helix::updateNonModeratorChatDelay(
    QString broadcasterID, QString moderatorID,
    std::optional<int> nonModeratorChatDelayDuration,
    ResultCallback<HelixChatSettings> successCallback,
    FailureCallback<HelixUpdateChatSettingsError, QString> failureCallback)
{
    QJsonObject json;
    json["non_moderator_chat_delay"] =
        nonModeratorChatDelayDuration.has_value();
    if (nonModeratorChatDelayDuration)
    {
        json["non_moderator_chat_delay_duration"] =
            *nonModeratorChatDelayDuration;
    }

    this->updateChatSettings(broadcasterID, moderatorID, json, successCallback,
                             failureCallback);
}

void Helix::updateSlowMode(
    QString broadcasterID, QString moderatorID,
    std::optional<int> slowModeWaitTime,
    ResultCallback<HelixChatSettings> successCallback,
    FailureCallback<HelixUpdateChatSettingsError, QString> failureCallback)
{
    QJsonObject json;
    json["slow_mode"] = slowModeWaitTime.has_value();
    if (slowModeWaitTime)
    {
        json["slow_mode_wait_time"] = *slowModeWaitTime;
    }

    this->updateChatSettings(broadcasterID, moderatorID, json, successCallback,
                             failureCallback);
}

void Helix::updateSubscriberMode(
    QString broadcasterID, QString moderatorID, bool subscriberMode,
    ResultCallback<HelixChatSettings> successCallback,
    FailureCallback<HelixUpdateChatSettingsError, QString> failureCallback)
{
    QJsonObject json;
    json["subscriber_mode"] = subscriberMode;
    this->updateChatSettings(broadcasterID, moderatorID, json, successCallback,
                             failureCallback);
}

void Helix::updateUniqueChatMode(
    QString broadcasterID, QString moderatorID, bool uniqueChatMode,
    ResultCallback<HelixChatSettings> successCallback,
    FailureCallback<HelixUpdateChatSettingsError, QString> failureCallback)
{
    QJsonObject json;
    json["unique_chat_mode"] = uniqueChatMode;
    this->updateChatSettings(broadcasterID, moderatorID, json, successCallback,
                             failureCallback);
}

void Helix::updateChatSettings(
    QString broadcasterID, QString moderatorID, QJsonObject payload,
    ResultCallback<HelixChatSettings> successCallback,
    FailureCallback<HelixUpdateChatSettingsError, QString> failureCallback)
{
    using Error = HelixUpdateChatSettingsError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("moderator_id", moderatorID);

    this->makePatch("chat/settings", urlQuery)
        .json(payload)
        .onSuccess([successCallback](auto result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for updating chat settings was"
                    << result.formatError() << "but we expected it to be 200";
            }
            auto response = result.parseJson();
            successCallback(HelixChatSettings(
                response.value("data").toArray().first().toObject()));
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.contains("must be in the range"))
                    {
                        failureCallback(Error::OutOfRange, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;
                case 409:
                case 422:
                case 425: {
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 403: {
                    failureCallback(Error::UserNotAuthorized, message);
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error updating chat settings:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::onFetchChattersSuccess(
    std::shared_ptr<HelixChatters> finalChatters, QString broadcasterID,
    QString moderatorID, size_t maxChattersToFetch,
    ResultCallback<HelixChatters> successCallback,
    FailureCallback<HelixGetChattersError, QString> failureCallback,
    HelixChatters chatters)
{
    qCDebug(chatterinoTwitch)
        << "Fetched" << chatters.chatters.size() << "chatters";

    finalChatters->chatters.merge(chatters.chatters);
    finalChatters->total = chatters.total;

    if (chatters.cursor.isEmpty() ||
        finalChatters->chatters.size() >= maxChattersToFetch)
    {
        successCallback(*finalChatters);
        return;
    }

    this->fetchChatters(
        broadcasterID, moderatorID, NUM_CHATTERS_TO_FETCH, chatters.cursor,
        [=, this](auto chatters) {
            this->onFetchChattersSuccess(
                finalChatters, broadcasterID, moderatorID, maxChattersToFetch,
                successCallback, failureCallback, chatters);
        },
        failureCallback);
}

void Helix::fetchChatters(
    QString broadcasterID, QString moderatorID, int first, QString after,
    ResultCallback<HelixChatters> successCallback,
    FailureCallback<HelixGetChattersError, QString> failureCallback)
{
    using Error = HelixGetChattersError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("moderator_id", moderatorID);
    urlQuery.addQueryItem("first", QString::number(first));

    if (!after.isEmpty())
    {
        urlQuery.addQueryItem("after", after);
    }

    this->makeGet("chat/chatters", urlQuery)
        .onSuccess([successCallback](auto result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for getting chatters was "
                    << result.formatError() << "but we expected it to be 200";
            }

            auto response = result.parseJson();
            successCallback(HelixChatters(response));
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.contains("OAuth token"))
                    {
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 403: {
                    failureCallback(Error::UserNotAuthorized, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error data:" << result.formatError()
                        << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::onFetchModeratorsSuccess(
    std::shared_ptr<std::vector<HelixModerator>> finalModerators,
    QString broadcasterID, size_t maxModeratorsToFetch,
    ResultCallback<std::vector<HelixModerator>> successCallback,
    FailureCallback<HelixGetModeratorsError, QString> failureCallback,
    HelixModerators moderators)
{
    qCDebug(chatterinoTwitch)
        << "Fetched " << moderators.moderators.size() << " moderators";

    std::for_each(moderators.moderators.begin(), moderators.moderators.end(),
                  [finalModerators](auto mod) {
                      finalModerators->push_back(mod);
                  });

    if (moderators.cursor.isEmpty() ||
        finalModerators->size() >= maxModeratorsToFetch)
    {
        successCallback(*finalModerators);
        return;
    }

    this->fetchModerators(
        broadcasterID, NUM_MODERATORS_TO_FETCH_PER_REQUEST, moderators.cursor,
        [=, this](auto moderators) {
            this->onFetchModeratorsSuccess(
                finalModerators, broadcasterID, maxModeratorsToFetch,
                successCallback, failureCallback, moderators);
        },
        failureCallback);
}

void Helix::fetchModerators(
    QString broadcasterID, int first, QString after,
    ResultCallback<HelixModerators> successCallback,
    FailureCallback<HelixGetModeratorsError, QString> failureCallback)
{
    using Error = HelixGetModeratorsError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("first", QString::number(first));

    if (!after.isEmpty())
    {
        urlQuery.addQueryItem("after", after);
    }

    this->makeGet("moderation/moderators", urlQuery)
        .onSuccess([successCallback](auto result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for getting moderators was "
                    << result.formatError() << "but we expected it to be 200";
            }

            auto response = result.parseJson();
            successCallback(HelixModerators(response));
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.contains("OAuth token"))
                    {
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 403: {
                    failureCallback(Error::UserNotAuthorized, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error data:" << result.formatError()
                        << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::banUser(QString broadcasterID, QString moderatorID, QString userID,
                    std::optional<int> duration, QString reason,
                    ResultCallback<> successCallback,
                    FailureCallback<HelixBanUserError, QString> failureCallback)
{
    using Error = HelixBanUserError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("moderator_id", moderatorID);

    QJsonObject payload;
    {
        QJsonObject data;
        data["reason"] = reason;
        data["user_id"] = userID;
        if (duration)
        {
            data["duration"] = *duration;
        }

        payload["data"] = data;
    }

    this->makePost("moderation/bans", urlQuery)
        .json(payload)
        .onSuccess([successCallback](auto result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for banning a user was"
                    << result.formatError() << "but we expected it to be 200";
            }

            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.startsWith("The user specified in the user_id "
                                           "field is already banned",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::TargetBanned, message);
                    }
                    else if (message.startsWith(
                                 "The user specified in the user_id field may "
                                 "not be banned",
                                 Qt::CaseInsensitive))
                    {
                        failureCallback(Error::CannotBanUser, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 409: {
                    failureCallback(Error::ConflictingOperation, message);
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 403: {
                    failureCallback(Error::UserNotAuthorized, message);
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error banning user:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::warnUser(
    QString broadcasterID, QString moderatorID, QString userID, QString reason,
    ResultCallback<> successCallback,
    FailureCallback<HelixWarnUserError, QString> failureCallback)
{
    using Error = HelixWarnUserError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("moderator_id", moderatorID);

    QJsonObject payload;
    {
        QJsonObject data;
        data["reason"] = reason;
        data["user_id"] = userID;

        payload["data"] = data;
    }

    this->makePost("moderation/warnings", urlQuery)
        .json(payload)
        .onSuccess([successCallback](auto result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for warning a user was"
                    << result.formatError() << "but we expected it to be 200";
            }

            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.startsWith("The user specified in the user_id "
                                           "field may not be warned",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::CannotWarnUser, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 403: {
                    failureCallback(Error::UserNotAuthorized, message);
                }
                break;

                case 409: {
                    failureCallback(Error::ConflictingOperation, message);
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error warning user:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::addSuspiciousUser(const QString broadcasterID,
                              const QString moderatorID, const QString userID,
                              const bool restricted,
                              ResultCallback<> successCallback,
                              FailureCallback<QString> failureCallback)
{
    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("moderator_id", moderatorID);

    QJsonObject payload;
    payload["user_id"] = userID;
    payload["status"] = restricted ? "RESTRICTED" : "ACTIVE_MONITORING";

    this->makePost("moderation/suspicious_users", urlQuery)
        .json(payload)
        .onSuccess([successCallback](const auto &result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for treating a suspicious user was"
                    << result.formatError() << "but we expected it to be 200";
            }

            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();
            if (!message.isEmpty())
            {
                failureCallback(message);
            }
            else
            {
                failureCallback(result.formatError());
            }
        })
        .execute();
}

void Helix::removeSuspiciousUser(const QString broadcasterID,
                                 const QString moderatorID,
                                 const QString userID,
                                 ResultCallback<> successCallback,
                                 FailureCallback<QString> failureCallback)
{
    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("moderator_id", moderatorID);
    urlQuery.addQueryItem("user_id", userID);

    this->makeDelete("moderation/suspicious_users", urlQuery)
        .onSuccess([successCallback](const auto &result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for un-treating a suspicious user was"
                    << result.formatError() << "but we expected it to be 200";
            }

            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();
            if (!message.isEmpty())
            {
                failureCallback(message);
            }
            else
            {
                failureCallback(result.formatError());
            }
        })
        .execute();
}

void Helix::sendWhisper(
    QString fromUserID, QString toUserID, QString message,
    ResultCallback<> successCallback,
    FailureCallback<HelixWhisperError, QString> failureCallback)
{
    using Error = HelixWhisperError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("from_user_id", fromUserID);
    urlQuery.addQueryItem("to_user_id", toUserID);

    QJsonObject payload;
    payload["message"] = message;

    this->makePost("whispers", urlQuery)
        .json(payload)
        .onSuccess([successCallback](auto result) {
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for sending a whisper was"
                    << result.formatError() << "but we expected it to be 204";
            }

            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.startsWith("A user cannot whisper themself",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::WhisperSelf, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.startsWith("the sender does not have a "
                                                "verified phone number",
                                                Qt::CaseInsensitive))
                    {
                        failureCallback(Error::NoVerifiedPhone, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 403: {
                    if (message.startsWith("The recipient's settings prevent "
                                           "this sender from whispering them",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::RecipientBlockedUser, message);
                    }
                    else
                    {
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                }
                break;

                case 404: {
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error banning user:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::getChatters(
    QString broadcasterID, QString moderatorID, size_t maxChattersToFetch,
    ResultCallback<HelixChatters> successCallback,
    FailureCallback<HelixGetChattersError, QString> failureCallback)
{
    auto finalChatters = std::make_shared<HelixChatters>();

    this->fetchChatters(
        broadcasterID, moderatorID, NUM_CHATTERS_TO_FETCH, "",
        [=, this](auto chatters) {
            this->onFetchChattersSuccess(
                finalChatters, broadcasterID, moderatorID, maxChattersToFetch,
                successCallback, failureCallback, chatters);
        },
        failureCallback);
}

void Helix::getModerators(
    QString broadcasterID, int maxModeratorsToFetch,
    ResultCallback<std::vector<HelixModerator>> successCallback,
    FailureCallback<HelixGetModeratorsError, QString> failureCallback)
{
    auto finalModerators = std::make_shared<std::vector<HelixModerator>>();

    this->fetchModerators(
        broadcasterID, NUM_MODERATORS_TO_FETCH_PER_REQUEST, "",
        [=, this](auto moderators) {
            this->onFetchModeratorsSuccess(
                finalModerators, broadcasterID, maxModeratorsToFetch,
                successCallback, failureCallback, moderators);
        },
        failureCallback);
}

void Helix::getChannelVIPs(
    QString broadcasterID,
    ResultCallback<std::vector<HelixVip>> successCallback,
    FailureCallback<HelixListVIPsError, QString> failureCallback)
{
    using Error = HelixListVIPsError;
    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);

    urlQuery.addQueryItem("first", "100");

    this->makeGet("channels/vips", urlQuery)
        .header("Content-Type", "application/json")
        .onSuccess([successCallback](auto result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for getting VIPs was"
                    << result.formatError() << "but we expected it to be 200";
            }

            auto response = result.parseJson();

            std::vector<HelixVip> channelVips;
            for (const auto &jsonStream : response.value("data").toArray())
            {
                channelVips.emplace_back(jsonStream.toObject());
            }

            successCallback(channelVips);
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.compare(
                                 "The ID in broadcaster_id must match the user "
                                 "ID found in the request's OAuth token.",
                                 Qt::CaseInsensitive) == 0)
                    {
                        failureCallback(Error::UserNotBroadcaster, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 403: {
                    failureCallback(Error::UserNotAuthorized, message);
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error listing VIPs:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::startCommercial(
    QString broadcasterID, int length,
    ResultCallback<HelixStartCommercialResponse> successCallback,
    FailureCallback<HelixStartCommercialError, QString> failureCallback)
{
    using Error = HelixStartCommercialError;

    QJsonObject payload;

    payload.insert("broadcaster_id", QJsonValue(broadcasterID));
    payload.insert("length", QJsonValue(length));

    this->makePost("channels/commercial", QUrlQuery())
        .json(payload)
        .onSuccess([successCallback, failureCallback](auto result) {
            auto obj = result.parseJson();
            if (obj.isEmpty())
            {
                failureCallback(
                    Error::Unknown,
                    "Twitch didn't send any information about this error.");
                return;
            }

            successCallback(HelixStartCommercialResponse(obj));
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.contains(
                                 "To start a commercial, the broadcaster must "
                                 "be streaming live.",
                                 Qt::CaseInsensitive))
                    {
                        failureCallback(Error::BroadcasterNotStreaming,
                                        message);
                    }
                    else if (message.startsWith("Missing required parameter",
                                                Qt::CaseInsensitive))
                    {
                        failureCallback(Error::MissingLengthParameter, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 401: {
                    if (message.contains(
                            "The ID in broadcaster_id must match the user ID "
                            "found in the request's OAuth token.",
                            Qt::CaseInsensitive))
                    {
                        failureCallback(Error::TokenMustMatchBroadcaster,
                                        message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error starting commercial:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::getGlobalBadges(
    ResultCallback<HelixGlobalBadges> successCallback,
    FailureCallback<HelixGetGlobalBadgesError, QString> failureCallback)
{
    using Error = HelixGetGlobalBadgesError;

    this->makeGet("chat/badges/global", QUrlQuery())
        .onSuccess([successCallback](auto result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for getting global badges was "
                    << result.formatError() << "but we expected it to be 200";
            }

            auto response = result.parseJson();
            successCallback(HelixGlobalBadges(response));
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 401: {
                    failureCallback(Error::Forwarded, message);
                }
                break;

                default: {
                    qCWarning(chatterinoTwitch)
                        << "Helix global badges, unhandled error data:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::getChannelBadges(
    QString broadcasterID, ResultCallback<HelixChannelBadges> successCallback,
    FailureCallback<HelixGetChannelBadgesError, QString> failureCallback)
{
    using Error = HelixGetChannelBadgesError;

    QUrlQuery urlQuery;
    urlQuery.addQueryItem("broadcaster_id", broadcasterID);

    this->makeGet("chat/badges", urlQuery)
        .onSuccess([successCallback](auto result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for getting badges was "
                    << result.formatError() << "but we expected it to be 200";
            }

            auto response = result.parseJson();
            successCallback(HelixChannelBadges(response));
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400:
                case 401: {
                    failureCallback(Error::Forwarded, message);
                }
                break;

                default: {
                    qCWarning(chatterinoTwitch)
                        << "Helix channel badges, unhandled error data:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::updateShieldMode(
    QString broadcasterID, QString moderatorID, bool isActive,
    ResultCallback<HelixShieldModeStatus> successCallback,
    FailureCallback<HelixUpdateShieldModeError, QString> failureCallback)
{
    using Error = HelixUpdateShieldModeError;

    QUrlQuery urlQuery;
    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("moderator_id", moderatorID);

    QJsonObject payload;
    payload["is_active"] = isActive;

    this->makePut("moderation/shield_mode", urlQuery)
        .json(payload)
        .onSuccess([successCallback](auto result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for updating shield mode was "
                    << result.formatError() << "but we expected it to be 200";
            }

            const auto response = result.parseJson();
            successCallback(
                HelixShieldModeStatus(response["data"][0].toObject()));
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            const auto obj = result.parseJson();
            auto message = obj["message"].toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                        break;
                    }

                    failureCallback(Error::Forwarded, message);
                }
                break;
                case 401: {
                    failureCallback(Error::Forwarded, message);
                }
                break;
                case 403: {
                    if (message.startsWith(
                            "Requester does not have permissions",
                            Qt::CaseInsensitive))
                    {
                        failureCallback(Error::MissingPermission, message);
                        break;
                    }
                }

                default: {
                    qCWarning(chatterinoTwitch)
                        << "Helix shield mode, unhandled error data:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::sendShoutout(
    QString fromBroadcasterID, QString toBroadcasterID, QString moderatorID,
    ResultCallback<> successCallback,
    FailureCallback<HelixSendShoutoutError, QString> failureCallback)
{
    using Error = HelixSendShoutoutError;

    QUrlQuery urlQuery;
    urlQuery.addQueryItem("from_broadcaster_id", fromBroadcasterID);
    urlQuery.addQueryItem("to_broadcaster_id", toBroadcasterID);
    urlQuery.addQueryItem("moderator_id", moderatorID);

    this->makePost("chat/shoutouts", urlQuery)
        .header("Content-Type", "application/json")
        .onSuccess([successCallback](NetworkResult result) {
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for sending shoutout was "
                    << result.formatError() << "but we expected it to be 204";
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            const auto obj = result.parseJson();
            auto message = obj["message"].toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.startsWith("The broadcaster may not give "
                                           "themselves a Shoutout.",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserIsBroadcaster, message);
                    }
                    else if (message.startsWith(
                                 "The broadcaster is not streaming live or "
                                 "does not have one or more viewers.",
                                 Qt::CaseInsensitive))
                    {
                        failureCallback(Error::BroadcasterNotLive, message);
                    }
                    else
                    {
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else
                    {
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                }
                break;

                case 403: {
                    failureCallback(Error::UserNotAuthorized, message);
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                case 500: {
                    if (message.isEmpty())
                    {
                        failureCallback(Error::Unknown,
                                        "Twitch internal server error");
                    }
                    else
                    {
                        failureCallback(Error::Unknown, message);
                    }
                }
                break;

                default: {
                    qCWarning(chatterinoTwitch)
                        << "Helix send shoutout, unhandled error data:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
            }
        })
        .execute();
}

void Helix::sendChatMessage(
    HelixSendMessageArgs args, ResultCallback<HelixSentMessage> successCallback,
    FailureCallback<HelixSendMessageError, QString> failureCallback)
{
    using Error = HelixSendMessageError;

    QJsonObject json{{
        {"broadcaster_id", args.broadcasterID},
        {"sender_id", args.senderID},
        {"message", args.message},
    }};
    if (!args.replyParentMessageID.isEmpty())
    {
        json["reply_parent_message_id"] = args.replyParentMessageID;
    }

    this->makePost("chat/messages", {})
        .json(json)
        .onSuccess([successCallback](const NetworkResult &result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for sending chat message was "
                    << result.formatError() << "but we expected it to be 200";
            }
            auto json = result.parseJson();

            successCallback(HelixSentMessage(
                json.value("data").toArray().at(0).toObject()));
        })
        .onError([failureCallback](const NetworkResult &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            const auto obj = result.parseJson();
            auto message = obj["message"].toString();

            if (message.isEmpty())
            {
                message = u"Twitch internal server error (" %
                          result.formatError() % ')';
            }

            switch (*result.status())
            {
                case 400: {
                    failureCallback(Error::Unknown, message);
                }
                break;

                case 401: {
                    if (message.startsWith("User access token requires the",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 403: {
                    failureCallback(Error::Forbidden, message);
                }
                break;

                case 422: {
                    failureCallback(Error::MessageTooLarge, message);
                }
                break;

                case 500: {
                    failureCallback(Error::Unknown, message);
                }
                break;

                default: {
                    qCWarning(chatterinoTwitch)
                        << "Helix send chat message, unhandled error data:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
            }
        })
        .execute();
}

void Helix::getUserEmotes(
    QString userID, QString broadcasterID,
    ResultCallback<std::vector<HelixChannelEmote>, HelixPaginationState>
        pageCallback,
    FailureCallback<QString> failureCallback, CancellationToken &&token)
{
    QUrlQuery query{{u"user_id"_s, userID}};
    if (!broadcasterID.isEmpty())
    {
        query.addQueryItem(u"broadcaster_id"_s, broadcasterID);
    }

    this->paginate(
        u"chat/emotes/user"_s, query,
        [pageCallback](const QJsonObject &json, const auto &state) mutable {
            const auto data = json["data"_L1].toArray();

            std::vector<HelixChannelEmote> emotes;
            emotes.reserve(data.count());

            for (const auto &emote : data)
            {
                emotes.emplace_back(emote.toObject());
            }

            pageCallback(emotes, state);

            return true;
        },
        [failureCallback](const NetworkResult &result) {
            if (!result.status())
            {
                failureCallback(result.formatError());
                return;
            }

            const auto obj = result.parseJson();
            auto message = obj["message"].toString();

            switch (*result.status())
            {
                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback("Missing required scope. Re-login with "
                                        "your account and try again.");
                        break;
                    }
                    [[fallthrough]];
                }
                default: {
                    qCWarning(chatterinoTwitch)
                        << "Helix get user emotes, unhandled error data:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(message);
                }
            }
        },
        std::move(token));
}

void Helix::getFollowedChannel(
    QString userID, QString broadcasterID, const QObject *caller,
    ResultCallback<std::optional<HelixFollowedChannel>> successCallback,
    FailureCallback<QString> failureCallback)
{
    this->makeGet("channels/followed",
                  {
                      {u"user_id"_s, userID},
                      {u"broadcaster_id"_s, broadcasterID},
                  })
        .caller(caller)
        .onSuccess([successCallback](auto result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for getting badges was "
                    << result.formatError() << "but we expected it to be 200";
            }

            const auto response = result.parseJson();
            const auto channel = response["data"_L1].toArray().at(0);
            if (channel.isObject())
            {
                successCallback(HelixFollowedChannel(channel.toObject()));
            }
            else
            {
                successCallback(std::nullopt);
            }
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();
            if (!message.isEmpty())
            {
                failureCallback(message);
            }
            else
            {
                failureCallback(result.formatError());
            }
        })
        .execute();
}

void Helix::createPoll(QString broadcasterID, QString title,
                       QStringList choices, const std::chrono::seconds duration,
                       const int pointsPerVote,
                       ResultCallback<> successCallback,
                       FailureCallback<QString> failureCallback)
{
    QJsonArray choiceArray;
    for (auto choice : choices)
    {
        choiceArray.append(QJsonObject{{{"title", choice}}});
    }

    QJsonObject json{{{"broadcaster_id", broadcasterID},
                      {"title", title},
                      {"duration", static_cast<int>(duration.count())},
                      {"choices", choiceArray}}};

    if (pointsPerVote > 0)
    {
        json["channel_points_voting_enabled"] = true;
        json["channel_points_per_vote"] = static_cast<qint64>(pointsPerVote);
    }

    this->makePost("polls", {})
        .json(json)
        .onSuccess([successCallback](const NetworkResult &result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for creating a poll was "
                    << result.formatError() << "but we expected it to be 200";
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) -> void {
            if (!result.status())
            {
                failureCallback(result.formatError());
                return;
            }

            const auto obj = result.parseJson();
            const auto message = obj.value("message").toString();
            if (!message.isEmpty())
            {
                failureCallback(message);
            }
            else
            {
                failureCallback(result.formatError());
            }
        })
        .execute();
}

void Helix::getPolls(const QString broadcasterID, QStringList ids,
                     const int first, const QString after,
                     ResultCallback<HelixPolls> successCallback,
                     FailureCallback<QString> failureCallback)
{
    QUrlQuery urlQuery;
    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("first", QString::number(first));

    if (!after.isEmpty())
    {
        urlQuery.addQueryItem("after", after);
    }

    for (const auto &id : ids)
    {
        urlQuery.addQueryItem("id", id);
    }

    this->makeGet("polls", urlQuery)
        .onSuccess([successCallback](const auto &result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for getting polls was "
                    << result.formatError() << "but we expected it to be 200";
            }

            const auto response = result.parseJson();
            successCallback(HelixPolls(response));
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();
            if (!message.isEmpty())
            {
                failureCallback(message);
            }
            else
            {
                failureCallback(result.formatError());
            }
        })
        .execute();
}

void Helix::endPoll(const QString broadcasterID, const QString id,
                    const bool immediatelyHide,
                    ResultCallback<HelixPoll> successCallback,
                    FailureCallback<QString> failureCallback)
{
    QJsonObject payload;
    payload.insert("broadcaster_id", broadcasterID);
    payload.insert("id", id);
    payload.insert("status", immediatelyHide ? "ARCHIVED" : "TERMINATED");

    this->makePatch("polls", {})
        .json(payload)
        .onSuccess([successCallback](const NetworkResult &result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for ending a poll was "
                    << result.formatError() << "but we expected it to be 200";
            }

            const auto response = result.parseJson();
            const auto data = HelixPolls(response);
            successCallback(data.polls.front());
        })
        .onError([failureCallback](const NetworkResult &result) -> void {
            if (!result.status())
            {
                failureCallback(result.formatError());
                return;
            }

            const auto obj = result.parseJson();
            const auto message = obj.value("message").toString();
            if (!message.isEmpty())
            {
                failureCallback(message);
            }
            else
            {
                failureCallback(result.formatError());
            }
        })
        .execute();
}

void Helix::createPrediction(const QString broadcasterID, const QString title,
                             QStringList outcomes,
                             const std::chrono::seconds duration,
                             ResultCallback<> successCallback,
                             FailureCallback<QString> failureCallback)
{
    QJsonArray outcomeArray;
    for (auto outcome : outcomes)
    {
        outcomeArray.append(QJsonObject{{{"title", outcome}}});
    }

    QJsonObject payload;
    payload.insert("broadcaster_id", broadcasterID);
    payload.insert("title", title);
    payload.insert("prediction_window", static_cast<int>(duration.count()));
    payload.insert("outcomes", outcomeArray);

    this->makePost("predictions", {})
        .json(payload)
        .onSuccess([successCallback](const NetworkResult &result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for creating a prediction was "
                    << result.formatError() << "but we expected it to be 200";
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) -> void {
            if (!result.status())
            {
                failureCallback(result.formatError());
                return;
            }

            const auto obj = result.parseJson();
            const auto message = obj.value("message").toString();
            if (!message.isEmpty())
            {
                failureCallback(message);
            }
            else
            {
                failureCallback(result.formatError());
            }
        })
        .execute();
}

void Helix::getPredictions(const QString broadcasterID, QStringList ids,
                           const int first, const QString after,
                           ResultCallback<HelixPredictions> successCallback,
                           FailureCallback<QString> failureCallback)
{
    QUrlQuery urlQuery;
    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("first", QString::number(first));

    if (!after.isEmpty())
    {
        urlQuery.addQueryItem("after", after);
    }

    for (const auto &id : ids)
    {
        urlQuery.addQueryItem("id", id);
    }

    this->makeGet("predictions", urlQuery)
        .onSuccess([successCallback](const auto &result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for getting predictions was "
                    << result.formatError() << "but we expected it to be 200";
            }

            const auto response = result.parseJson();
            successCallback(HelixPredictions(response));
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();
            if (!message.isEmpty())
            {
                failureCallback(message);
            }
            else
            {
                failureCallback(result.formatError());
            }
        })
        .execute();
}

void Helix::endPrediction(const QString broadcasterID, const QString id,
                          const bool refundPoints,
                          const QString winningOutcomeID,
                          ResultCallback<HelixPrediction> successCallback,
                          FailureCallback<QString> failureCallback)
{
    QJsonObject payload;
    payload.insert("broadcaster_id", broadcasterID);
    payload.insert("id", id);
    if (refundPoints)
    {
        payload.insert("status", "CANCELED");
    }
    else if (winningOutcomeID.isEmpty())
    {
        payload.insert("status", "LOCKED");
    }
    else
    {
        payload.insert("status", "RESOLVED");
        payload.insert("winning_outcome_id", winningOutcomeID);
    }

    this->makePatch("predictions", {})
        .json(payload)
        .onSuccess([successCallback,
                    failureCallback](const NetworkResult &result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for ending a prediction was "
                    << result.formatError() << "but we expected it to be 200";
            }

            const auto response = result.parseJson();
            const auto data = HelixPredictions(response);
            if (data.predictions.empty())
            {
                qCWarning(chatterinoTwitch) << "Prediction end response did "
                                               "not contain any predictions";
                failureCallback("Twitch API Error: empty prediction response");
                return;
            }
            successCallback(data.predictions.front());
        })
        .onError([failureCallback](const NetworkResult &result) -> void {
            if (!result.status())
            {
                failureCallback(result.formatError());
                return;
            }

            const auto obj = result.parseJson();
            const auto message = obj.value("message").toString();
            if (!message.isEmpty())
            {
                failureCallback(message);
            }
            else
            {
                failureCallback(result.formatError());
            }
        })
        .execute();
}

void Helix::createEventSubSubscription(
    const eventsub::SubscriptionRequest &request, const QString &sessionID,
    ResultCallback<HelixCreateEventSubSubscriptionResponse> successCallback,
    FailureCallback<HelixCreateEventSubSubscriptionError, QString>
        failureCallback,
    const QString &clientIdOverride, const QString &oauthTokenOverride)
{
    using Error = HelixCreateEventSubSubscriptionError;

    QJsonObject condition;
    for (const auto &[conditionKey, conditionValue] : request.conditions)
    {
        condition.insert(conditionKey, conditionValue);
    }

    QJsonObject body;
    body.insert("type", request.subscriptionType);
    body.insert("version", request.subscriptionVersion);
    body.insert("condition", condition);

    QJsonObject transport;
    transport.insert("method", "websocket");
    transport.insert("session_id", sessionID);

    body.insert("transport", transport);

    const auto useOverride = !oauthTokenOverride.isEmpty();
    auto postRequest =
        useOverride
            ? this->makePost("eventsub/subscriptions", {}, clientIdOverride,
                             oauthTokenOverride)
            : this->makePost("eventsub/subscriptions", {});

    std::move(postRequest)
        .json(body)
        .onSuccess([successCallback](const auto &result) {
            if (result.status() != 202)
            {
                qCWarning(chatterinoTwitchEventSub)
                    << "Success result for creating eventsub subscription was "
                    << result.formatError() << "but we expected it to be 202";
            }

            HelixCreateEventSubSubscriptionResponse response(
                result.parseJson());

            successCallback(response);
        })
        .onError([failureCallback](const NetworkResult &result) {
            if (!result.status())
            {
                failureCallback(Error::Forwarded, result.formatError());
                return;
            }

            const auto obj = result.parseJson();
            auto message = obj["message"].toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.startsWith(
                            "websocket transport session does not exist",
                            Qt::CaseInsensitive))
                    {
                        failureCallback(Error::NoSession, message);
                    }
                    else
                    {
                        failureCallback(Error::BadRequest, message);
                    }
                }
                break;

                case 401: {
                    failureCallback(Error::Unauthorized, message);
                }
                break;

                case 403: {
                    failureCallback(Error::Forbidden, message);
                }
                break;
                case 409: {
                    failureCallback(Error::Conflict, message);
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                case 500: {
                    if (message.isEmpty())
                    {
                        failureCallback(Error::Forwarded,
                                        "Twitch internal server error");
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                default: {
                    qCWarning(chatterinoTwitchEventSub)
                        << "Helix Create EventSub Subscription, unhandled "
                           "error data:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Forwarded, message);
                }
            }
        })
        .execute();
}

QDebug &operator<<(QDebug &dbg,
                   const HelixCreateEventSubSubscriptionResponse &data)
{
    dbg << "HelixCreateEventSubSubscriptionResponse{ id:" << data.subscriptionID
        << "status:" << data.subscriptionStatus
        << "type:" << data.subscriptionType
        << "version:" << data.subscriptionVersion
        << "condition:" << data.subscriptionCondition
        << "createdAt:" << data.subscriptionCreatedAt
        << "sessionID:" << data.subscriptionSessionID
        << "connectedAt:" << data.subscriptionConnectedAt
        << "cost:" << data.subscriptionCost << "total:" << data.total
        << "totalCost:" << data.totalCost
        << "maxTotalCost:" << data.maxTotalCost << '}';
    return dbg;
}

void Helix::deleteEventSubSubscription(const QString &subscriptionID,
                                       ResultCallback<> successCallback,
                                       FailureCallback<QString> failureCallback)
{
    QUrlQuery query;
    query.addQueryItem("id", subscriptionID);

    this->makeDelete("eventsub/subscriptions", query)
        .onSuccess([successCallback](const auto &result) {
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitchEventSub)
                    << "Success result for deleting eventsub subscription was "
                    << result.formatError() << "but we expected it to be 204";
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            if (!result.status())
            {
                failureCallback(result.formatError());
                return;
            }

            const auto obj = result.parseJson();
            auto message = obj["message"].toString();

            if (message.isEmpty())
            {
                failureCallback("Twitch internal server error");
            }
            else
            {
                failureCallback(message);
            }
        })
        .execute();
}

NetworkRequest Helix::makeRequest(const QString &url, const QUrlQuery &urlQuery,
                                  NetworkRequestType type)
{
    return this->makeRequest(url, urlQuery, type, this->clientId,
                             this->oauthToken);
}

NetworkRequest Helix::makeRequest(const QString &url, const QUrlQuery &urlQuery,
                                  NetworkRequestType type,
                                  const QString &clientId,
                                  const QString &oauthToken)
{
    assert(!url.startsWith("/"));

    if (clientId.isEmpty())
    {
        qCDebug(chatterinoTwitch)
            << "Helix::makeRequest called without a client ID set BabyRage";
    }

    if (oauthToken.isEmpty())
    {
        qCDebug(chatterinoTwitch)
            << "Helix::makeRequest called without an oauth token set BabyRage";
    }

    QString baseUrl("https://api.twitch.tv/helix/");

#ifndef NDEBUG
    bool ignoreSslErrors =
        getApp()->getArgs().useLocalEventsub && url == "eventsub/subscriptions";
    if (ignoreSslErrors)
    {
        baseUrl = "https://127.0.0.1:3012/";
    }
#endif

    QUrl fullUrl(baseUrl + url);

    fullUrl.setQuery(urlQuery);

    return NetworkRequest(fullUrl, type)
        .timeout(5 * 1000)
        .header("Accept", "application/json")
        .header("Client-ID", clientId)
        .header("Authorization", "Bearer " + oauthToken)
#ifndef NDEBUG
        .ignoreSslErrors(ignoreSslErrors)
#endif
        ;
}

NetworkRequest Helix::makeGet(const QString &url, const QUrlQuery &urlQuery)
{
    return this->makeRequest(url, urlQuery, NetworkRequestType::Get);
}

NetworkRequest Helix::makeDelete(const QString &url, const QUrlQuery &urlQuery)
{
    return this->makeRequest(url, urlQuery, NetworkRequestType::Delete);
}

NetworkRequest Helix::makePost(const QString &url, const QUrlQuery &urlQuery)
{
    return this->makePost(url, urlQuery, this->clientId, this->oauthToken);
}

NetworkRequest Helix::makePost(const QString &url, const QUrlQuery &urlQuery,
                               const QString &clientId,
                               const QString &oauthToken)
{
    return this->makeRequest(url, urlQuery, NetworkRequestType::Post, clientId,
                             oauthToken);
}

NetworkRequest Helix::makePut(const QString &url, const QUrlQuery &urlQuery)
{
    return this->makeRequest(url, urlQuery, NetworkRequestType::Put);
}

NetworkRequest Helix::makePatch(const QString &url, const QUrlQuery &urlQuery)
{
    return this->makeRequest(url, urlQuery, NetworkRequestType::Patch);
}

void Helix::paginate(
    const QString &url, const QUrlQuery &baseQuery,
    std::function<bool(const QJsonObject &, const HelixPaginationState &state)>
        onPage,
    std::function<void(NetworkResult)> onError,
    CancellationToken &&cancellationToken)
{
    auto onSuccess =
        std::make_shared<std::function<void(NetworkResult)>>(nullptr);

    auto onSuccessCb = [onSuccess](const auto &res) {
        return (*onSuccess)(res);
    };

    *onSuccess = [this, onPage = std::move(onPage), onError, onSuccessCb,
                  url{url}, baseQuery{baseQuery},
                  cancellationToken =
                      std::move(cancellationToken)](const NetworkResult &res) {
        if (cancellationToken.isCancelled())
        {
            return;
        }

        const auto json = res.parseJson();
        const auto pagination = json["pagination"_L1].toObject();

        auto cursor = pagination["cursor"_L1].toString();
        HelixPaginationState state{.done = cursor.isEmpty()};

        if (!onPage(json, state))
        {
            qCDebug(chatterinoTwitch)
                << "paginate onPage returned false for" << url;
            return;
        }

        if (state.done)
        {
            return;
        }

        auto query = baseQuery;
        query.removeAllQueryItems(u"after"_s);
        query.addQueryItem(u"after"_s, cursor);

        this->makeGet(url, query)
            .onSuccess(onSuccessCb)
            .onError(onError)
            .execute();
    };

    this->makeGet(url, baseQuery)
        .onSuccess(std::move(onSuccessCb))
        .onError(std::move(onError))
        .execute();
}

void Helix::update(QString clientId, QString oauthToken)
{
    this->clientId = std::move(clientId);
    this->oauthToken = std::move(oauthToken);
}

void Helix::initialize()
{
    assert(instance == nullptr);

    initializeHelix(new Helix());
}

void initializeHelix(IHelix *_instance)
{
    assert(_instance != nullptr);

    instance = _instance;
}

IHelix *getHelix()
{
    assert(instance != nullptr);

    return instance;
}

}  // namespace chatterino

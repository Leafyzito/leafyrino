// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/seventv/SeventvAPI.hpp"

#include "common/Literals.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"

#include <QJsonObject>

#include <functional>
#include <memory>
#include <utility>

namespace {

using namespace chatterino::literals;

const QString API_URL_USER = u"https://7tv.io/v3/users/twitch/%1"_s;
const QString API_URL_KICK_USER = u"https://7tv.io/v3/users/kick/%1"_s;
const QString API_URL_EMOTE_SET = u"https://7tv.io/v3/emote-sets/%1"_s;
const QString API_URL_PRESENCES = u"https://7tv.io/v3/users/%1/presences"_s;
constexpr int CHANNEL_USER_TIMEOUT_MS = 30000;

chatterino::NetworkRequest tuneSeventvRequest(
    chatterino::NetworkRequest &&request)
{
    return std::move(request)
        .timeout(30000);
}
using SeventvJsonCallback = std::function<void(const QJsonObject &)>;
using SeventvErrorCallback =
    std::function<void(const chatterino::NetworkResult &)>;

auto makeSharedErrorCallback(SeventvErrorCallback &&onError)
{
    return std::make_shared<SeventvErrorCallback>(std::move(onError));
}
auto makeJsonSuccessCallback(SeventvJsonCallback &&onSuccess,
                             std::shared_ptr<SeventvErrorCallback> onError)
{
    return [callback = std::move(onSuccess),
            onError = std::move(onError)](const chatterino::NetworkResult
                                              &result) {
        auto json = result.parseJson();
        if (json.isEmpty())
        {
            if (*onError)
            {
                (*onError)(result);
            }
            return;
        }

        callback(json);
    };
}

auto makeNetworkErrorCallback(std::shared_ptr<SeventvErrorCallback> onError)
{
    return [onError = std::move(onError)](const chatterino::NetworkResult
                                              &result) {
        if (*onError)
        {
            (*onError)(result);
        }
    };
}

}
namespace chatterino {

void SeventvAPI::getUserByTwitchID(
    const QString &twitchID, SuccessCallback<const QJsonObject &> &&onSuccess,
    ErrorCallback &&onError)
{
    auto sharedOnError = makeSharedErrorCallback(std::move(onError));
    tuneSeventvRequest(
        NetworkRequest(API_URL_USER.arg(twitchID), NetworkRequestType::Get))
        .timeout(CHANNEL_USER_TIMEOUT_MS)
        .onSuccess(makeJsonSuccessCallback(std::move(onSuccess),
                                           sharedOnError))
        .onError(makeNetworkErrorCallback(std::move(sharedOnError)))
        .execute();
}

void SeventvAPI::getUserByKickID(
    uint64_t userID, SuccessCallback<const QJsonObject &> &&onSuccess,
    ErrorCallback &&onError)
{
    auto sharedOnError = makeSharedErrorCallback(std::move(onError));
    tuneSeventvRequest(
        NetworkRequest(API_URL_KICK_USER.arg(userID), NetworkRequestType::Get))
        .timeout(CHANNEL_USER_TIMEOUT_MS)
        .onSuccess(makeJsonSuccessCallback(std::move(onSuccess),
                                           sharedOnError))
        .onError(makeNetworkErrorCallback(std::move(sharedOnError)))
        .execute();
}

void SeventvAPI::getEmoteSet(const QString &emoteSet,
                             SuccessCallback<const QJsonObject &> &&onSuccess,
                             ErrorCallback &&onError)
{
    auto sharedOnError = makeSharedErrorCallback(std::move(onError));
    tuneSeventvRequest(
        NetworkRequest(API_URL_EMOTE_SET.arg(emoteSet), NetworkRequestType::Get))
        .timeout(25000)
        .onSuccess(makeJsonSuccessCallback(std::move(onSuccess),
                                           sharedOnError))
        .onError(makeNetworkErrorCallback(std::move(sharedOnError)))
        .execute();
}

void SeventvAPI::updatePresence(const QString &twitchChannelID,
                                const QString &seventvUserID,
                                SuccessCallback<> &&onSuccess,
                                ErrorCallback &&onError)
{
    QJsonObject payload{
        {u"kind"_s, 1},
        {u"data"_s,
         QJsonObject{
             {u"id"_s, twitchChannelID},
             {u"platform"_s, u"TWITCH"_s},
         }},
    };

    tuneSeventvRequest(NetworkRequest(API_URL_PRESENCES.arg(seventvUserID),
                                      NetworkRequestType::Post))
        .json(payload)
        .timeout(10000)
        .onSuccess([callback = std::move(onSuccess)](const auto &) {
            callback();
        })
        .onError([callback = std::move(onError)](const NetworkResult &result) {
            callback(result);
        })
        .execute();
}

void SeventvAPI::updateKickPresence(uint64_t kickUserID,
                                    const QString &seventvUserID,
                                    SuccessCallback<> &&onSuccess,
                                    ErrorCallback &&onError)
{
    QJsonObject payload{
        {u"kind"_s, 1},
        {u"data"_s,
         QJsonObject{
             {u"id"_s, QString::number(kickUserID)},
             {u"platform"_s, u"KICK"_s},
         }},
    };

    tuneSeventvRequest(NetworkRequest(API_URL_PRESENCES.arg(seventvUserID),
                                      NetworkRequestType::Post))
        .json(payload)
        .timeout(10000)
        .onSuccess([callback = std::move(onSuccess)](const auto &) {
            callback();
        })
        .onError([callback = std::move(onError)](const NetworkResult &result) {
            callback(result);
        })
        .execute();
}

}

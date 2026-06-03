// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/IvrApi.hpp"

#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"

#include <QUrlQuery>

namespace chatterino {

static IvrApi *instance = nullptr;

void IvrApi::getSubage(QString userName, QString channelName,
                       ResultCallback<IvrSubage> successCallback,
                       IvrFailureCallback failureCallback)
{
    assert(!userName.isEmpty() && !channelName.isEmpty());

    this->makeRequest(
            QString("twitch/subage/%1/%2").arg(userName).arg(channelName), {})
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();

            successCallback(root);
        })
        .onError([failureCallback](auto result) {
            qCWarning(chatterinoIvr)
                << "Failed IVR API Call!" << result.formatError()
                << QString(result.getData());
            failureCallback();
        })
        .execute();
}

void IvrApi::getFounders(QString channelName,
                         ResultCallback<QJsonArray> successCallback,
                         IvrFailureCallback failureCallback)
{
    assert(!channelName.isEmpty());

    this->makeRequest(QString("twitch/founders/%1").arg(channelName), {})
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson().value("founders").toArray();

            successCallback(root);
        })
        .onError([failureCallback](auto result) {
            qCWarning(chatterinoIvr)
                << "Failed IVR API Call!" << result.formatError()
                << QString(result.getData());
            failureCallback();
        })
        .execute();
}

void IvrApi::getModVip(QString channelName,
                       ResultCallback<IvrModVip> successCallback,
                       IvrFailureCallback failureCallback)
{
    assert(!channelName.isEmpty());

    this->makeRequest(QString("twitch/modvip/%1").arg(channelName), {})
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();

            successCallback(root);
        })
        .onError([failureCallback](auto result) {
            qCWarning(chatterinoIvr)
                << "Failed IVR API Call!" << result.formatError()
                << QString(result.getData());
            failureCallback();
        })
        .execute();
}

void IvrApi::getUserRoles(QString userName,
                          ResultCallback<IvrResolve> successCallback,
                          IvrFailureCallback failureCallback)
{
    assert(!userName.isEmpty());

    this->makeRequest(QString("twitch/user"),
                      QUrlQuery(QString("login=%1").arg(userName)))
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJsonArray();

            successCallback(root);
        })
        .onError([failureCallback](auto result) {
            qCWarning(chatterinoIvr)
                << "Failed IVR API Call!" << result.formatError()
                << QString(result.getData());
            failureCallback();
        })
        .execute();
}

void IvrApi::getModVip(
    QString channelName,
    ResultCallback<std::vector<HelixModerator>, std::vector<HelixVip>>
        successCallback,
    IvrFailureCallback failureCallback)
{
    this->getModVip(
        std::move(channelName),
        [successCallback =
             std::move(successCallback)](const IvrModVip &modVip) mutable {
            std::vector<HelixModerator> mods;
            mods.reserve(modVip.mods.size());
            for (const auto &entry : modVip.mods)
            {
                const auto obj = entry.toObject();
                mods.emplace_back(QJsonObject{
                    {"user_id", obj.value("id").toString()},
                    {"user_login", obj.value("login").toString()},
                    {"user_name", obj.value("displayName").toString()},
                });
            }

            std::vector<HelixVip> vips;
            vips.reserve(modVip.vips.size());
            for (const auto &entry : modVip.vips)
            {
                const auto obj = entry.toObject();
                vips.emplace_back(QJsonObject{
                    {"user_id", obj.value("id").toString()},
                    {"user_login", obj.value("login").toString()},
                    {"user_name", obj.value("displayName").toString()},
                });
            }

            successCallback(std::move(mods), std::move(vips));
        },
        std::move(failureCallback));
}

void IvrApi::getFounders(
    QString channelName,
    ResultCallback<std::vector<HelixModerator>> successCallback,
    IvrFailureCallback failureCallback)
{
    this->getFounders(
        std::move(channelName),
        [successCallback = std::move(successCallback)](
            const QJsonArray &foundersArray) mutable {
            std::vector<HelixModerator> founders;
            founders.reserve(foundersArray.size());
            for (const auto &entry : foundersArray)
            {
                const auto obj = entry.toObject();
                founders.emplace_back(QJsonObject{
                    {"user_id", obj.value("id").toString()},
                    {"user_login", obj.value("login").toString()},
                    {"user_name", obj.value("displayName").toString()},
                });
            }

            successCallback(std::move(founders));
        },
        std::move(failureCallback));
}

void IvrApi::getUser(QString userName,
                     ResultCallback<IvrUserProfile> successCallback,
                     IvrFailureCallback failureCallback)
{
    assert(!userName.isEmpty());

    QUrlQuery query;
    query.addQueryItem("login", userName);

    this->makeRequest("twitch/user", query)
        .onSuccess([successCallback, failureCallback](auto result) {
            const auto root = result.parseJsonArray();
            if (root.isEmpty() || !root.first().isObject())
            {
                failureCallback();
                return;
            }

            successCallback(IvrUserProfile(root.first().toObject()));
        })
        .onError([failureCallback](auto result) {
            qCWarning(chatterinoIvr)
                << "Failed IVR user API call!" << result.formatError()
                << QString(result.getData());
            failureCallback();
        })
        .execute();
}

NetworkRequest IvrApi::makeRequest(QString url, QUrlQuery urlQuery)
{
    assert(!url.startsWith("/"));

    const QString baseUrl("https://api.ivr.fi/v2/");
    QUrl fullUrl(baseUrl + url);
    fullUrl.setQuery(urlQuery);

    return NetworkRequest(fullUrl).timeout(5 * 1000).header("Accept",
                                                            "application/json");
}

void IvrApi::initialize()
{
    assert(instance == nullptr);

    instance = new IvrApi();
}

IvrApi *getIvr()
{
    assert(instance != nullptr);

    return instance;
}

}  // namespace chatterino

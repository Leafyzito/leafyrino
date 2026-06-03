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

void IvrApi::getModVip(
    QString channelName,
    ResultCallback<std::vector<HelixModerator>, std::vector<HelixVip>>
        successCallback,
    IvrFailureCallback failureCallback)
{
    assert(!channelName.isEmpty());

    this->makeRequest(QString("twitch/modvip/%1").arg(channelName), {})
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();
            const auto modsValue = root.value("mods");
            const auto vipsValue = root.value("vips");

            if (!modsValue.isArray() || !vipsValue.isArray())
            {
                failureCallback();
                return;
            }

            std::vector<HelixModerator> mods;
            for (const auto &entry : modsValue.toArray())
            {
                const auto obj = entry.toObject();
                mods.emplace_back(QJsonObject{
                    {"user_id", obj.value("id").toString()},
                    {"user_login", obj.value("login").toString()},
                    {"user_name", obj.value("displayName").toString()},
                });
            }

            std::vector<HelixVip> vips;
            for (const auto &entry : vipsValue.toArray())
            {
                const auto obj = entry.toObject();
                vips.emplace_back(QJsonObject{
                    {"user_id", obj.value("id").toString()},
                    {"user_login", obj.value("login").toString()},
                    {"user_name", obj.value("displayName").toString()},
                });
            }

            successCallback(std::move(mods), std::move(vips));
        })
        .onError([failureCallback](auto result) {
            qCWarning(chatterinoIvr)
                << "Failed IVR modvip API call!" << result.formatError()
                << QString(result.getData());
            failureCallback();
        })
        .execute();
}

void IvrApi::getFounders(QString channelName,
                         ResultCallback<std::vector<HelixModerator>>
                             successCallback,
                         IvrFailureCallback failureCallback)
{
    assert(!channelName.isEmpty());

    this->makeRequest(QString("twitch/founders/%1").arg(channelName), {})
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();
            const auto foundersValue = root.value("founders");

            if (!foundersValue.isArray())
            {
                failureCallback();
                return;
            }

            std::vector<HelixModerator> founders;
            for (const auto &entry : foundersValue.toArray())
            {
                const auto obj = entry.toObject();
                founders.emplace_back(QJsonObject{
                    {"user_id", obj.value("id").toString()},
                    {"user_login", obj.value("login").toString()},
                    {"user_name", obj.value("displayName").toString()},
                });
            }

            successCallback(std::move(founders));
        })
        .onError([failureCallback](auto result) {
            qCWarning(chatterinoIvr)
                << "Failed IVR founders API call!" << result.formatError()
                << QString(result.getData());
            failureCallback();
        })
        .execute();
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

}

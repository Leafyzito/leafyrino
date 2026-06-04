// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/network/NetworkRequest.hpp"
#include "providers/twitch/api/Helix.hpp"

#include <QJsonArray>
#include <QJsonObject>

#include <functional>
#include <optional>
#include <vector>

namespace chatterino {

using IvrFailureCallback = std::function<void()>;
template <typename... T>
using ResultCallback = std::function<void(T...)>;

struct IvrSubage {
    const bool isSubHidden;
    const bool isSubbed;
    const QString subTier;
    const int totalSubMonths;
    const QString followingSince;
    const QString endsAt;
    const QString subType;
    const QString giftGifterDisplayName;
    const QString giftGifterLogin;

    IvrSubage(const QJsonObject &root)
        : isSubHidden(root.value("statusHidden").toBool())
        , isSubbed(!root.value("meta").isNull())
        , subTier(root.value("meta").toObject().value("tier").toString())
        , totalSubMonths(
              root.value("cumulative").toObject().value("months").toInt())
        , followingSince(root.value("followedAt").toString())
        , endsAt(root.value("meta").toObject().value("endsAt").toString())
        , subType(root.value("meta").toObject().value("type").toString())
        , giftGifterDisplayName(root.value("meta")
                                    .toObject()
                                    .value("giftMeta")
                                    .toObject()
                                    .value("gifter")
                                    .toObject()
                                    .value("displayName")
                                    .toString())
        , giftGifterLogin(root.value("meta")
                              .toObject()
                              .value("giftMeta")
                              .toObject()
                              .value("gifter")
                              .toObject()
                              .value("login")
                              .toString())
    {
    }

    bool isGiftSub() const
    {
        return this->subType == QStringLiteral("gift");
    }

    QString giftGifterName() const
    {
        if (!this->giftGifterDisplayName.isEmpty())
        {
            return this->giftGifterDisplayName;
        }

        return this->giftGifterLogin;
    }
};

struct IvrResolve {
    const bool isPartner;
    const bool isAffiliate;
    const bool isBot;
    const bool isStaff;
    const bool isExStaff;
    const int chatterCount;
    const QString lastBroadcastStartedAt;
    const QString chatColor;

    IvrResolve(QJsonArray arr)
        : isPartner(arr.at(0)
                        .toObject()
                        .value("roles")
                        .toObject()
                        .value("isPartner")
                        .toBool())
        , isAffiliate(arr.at(0)
                          .toObject()
                          .value("roles")
                          .toObject()
                          .value("isAffiliate")
                          .toBool())
        , isBot(arr.at(0).toObject().value("verifiedBot").toBool())
        , isStaff(arr.at(0)
                      .toObject()
                      .value("roles")
                      .toObject()
                      .value("isStaff")
                      .toBool())
        , isExStaff(!arr.at(0)
                         .toObject()
                         .value("roles")
                         .toObject()
                         .value("isStaff")
                         .isNull() &&
                    !arr.at(0)
                         .toObject()
                         .value("roles")
                         .toObject()
                         .value("isStaff")
                         .toBool() &&
                    !arr.at(0).isUndefined())
        , chatterCount(arr.at(0).toObject().value("chatterCount").isNull()
                           ? -1
                           : arr.at(0).toObject().value("chatterCount").toInt())
        , lastBroadcastStartedAt(arr.at(0)
                                     .toObject()
                                     .value("lastBroadcast")
                                     .toObject()
                                     .value("startedAt")
                                     .toString())
        , chatColor(arr.at(0).toObject().value("chatColor").toString())
    {
    }
};

struct IvrModVip {
    const QJsonArray mods;
    const QJsonArray vips;

    IvrModVip(const QJsonObject &root)
        : mods(root.value("mods").toArray())
        , vips(root.value("vips").toArray())
    {
    }
};

struct IvrUserProfile {
    bool banned = false;
    std::optional<int> chatterCount;
    QString chatColor;
    bool isAffiliate = false;
    bool isPartner = false;
    bool isStaff = false;
    QString lastBroadcastStartedAt;
    QString lastBroadcastTitle;

    IvrUserProfile() = default;
    explicit IvrUserProfile(const QJsonObject &root)
        : banned(root.value("banned").toBool())
        , chatColor(root.value("chatColor").toString())
    {
        const auto chatterCountValue = root.value("chatterCount");
        if (!chatterCountValue.isNull() && !chatterCountValue.isUndefined())
        {
            this->chatterCount = chatterCountValue.toInt();
        }

        const auto roles = root.value("roles").toObject();
        this->isAffiliate = roles.value("isAffiliate").toBool();
        this->isPartner = roles.value("isPartner").toBool();
        this->isStaff = roles.value("isStaff").toBool();

        const auto lastBroadcast = root.value("lastBroadcast").toObject();
        this->lastBroadcastStartedAt =
            lastBroadcast.value("startedAt").toString();
        this->lastBroadcastTitle = lastBroadcast.value("title").toString();
    }
};

class IvrApi final
{
public:
    void getSubage(QString userName, QString channelName,
                   ResultCallback<IvrSubage> resultCallback,
                   IvrFailureCallback failureCallback);

    // https://api.ivr.fi/v2/docs/static/index.html#/Twitch/get_twitch_user
    void getUserRoles(QString userName,
                      ResultCallback<IvrResolve> resultCallback,
                      IvrFailureCallback failureCallback);

    // https://api.ivr.fi/v2/docs/static/index.html#/Twitch/get_twitch_founders__login_
    void getFounders(QString channelName,
                     ResultCallback<QJsonArray> resultCallback,
                     IvrFailureCallback failureCallback);
    void getFounders(QString channelName,
                     ResultCallback<std::vector<HelixModerator>> resultCallback,
                     IvrFailureCallback failureCallback);

    // https://api.ivr.fi/v2/docs/static/index.html#/Twitch/get_twitch_modvip__channel_
    void getModVip(QString channelName,
                   ResultCallback<IvrModVip> resultCallback,
                   IvrFailureCallback failureCallback);
    void getModVip(
        QString channelName,
        ResultCallback<std::vector<HelixModerator>, std::vector<HelixVip>>
            resultCallback,
        IvrFailureCallback failureCallback);

    void getUser(QString userName,
                 ResultCallback<IvrUserProfile> resultCallback,
                 IvrFailureCallback failureCallback);

    static void initialize();

    IvrApi() = default;

    IvrApi(const IvrApi &) = delete;
    IvrApi &operator=(const IvrApi &) = delete;

    IvrApi(IvrApi &&) = delete;
    IvrApi &operator=(IvrApi &&) = delete;

private:
    NetworkRequest makeRequest(QString url, QUrlQuery urlQuery);
};

IvrApi *getIvr();

}  // namespace chatterino

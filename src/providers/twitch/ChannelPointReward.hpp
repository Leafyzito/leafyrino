// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/ImageSet.hpp"

#include <QJsonObject>

namespace chatterino {

struct ChannelPointReward {
    ChannelPointReward(const QJsonObject &redemption);
    ChannelPointReward() = delete;
    QString id;
    QString redemptionKey;
    QString channelId;
    QString title;
    QString prompt;
    int cost;
    ImageSet image;
    bool isUserInputRequired = false;
    bool isBits = false;
    QString emoteId;    // currently only for celebrations
    QString emoteName;  // currently only for celebrations

    struct {
        QString id;
        QString login;
        QString displayName;
    } user;

    /// Whether FFZ treats this reward as a giant emote reward (FFZ:GE in its
    /// title or prompt).
    bool isFfzGiantEmote() const;

    QJsonObject toJson() const;
};

}  // namespace chatterino

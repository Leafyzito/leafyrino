// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QJsonObject>

namespace chatterino {

struct BttvLiveUpdateEmoteUpdateAddMessage {
    BttvLiveUpdateEmoteUpdateAddMessage(const QJsonObject &json);

    QString channelID;

    QJsonObject jsonEmote;
    QString emoteName;
    QString emoteID;

    bool validate() const;

private:
    bool badChannelID_;
};

struct BttvLiveUpdateEmoteRemoveMessage {
    BttvLiveUpdateEmoteRemoveMessage(const QJsonObject &json);

    QString channelID;
    QString emoteID;

    bool validate() const;

private:
    bool badChannelID_;
};

struct BttvLiveUpdateUserUpdateMessage {
    BttvLiveUpdateUserUpdateMessage(const QJsonObject &json);

    QString userID;
    QJsonObject badgeObject;

    bool validate() const;
    bool hasBadge() const;
};

}  // namespace chatterino

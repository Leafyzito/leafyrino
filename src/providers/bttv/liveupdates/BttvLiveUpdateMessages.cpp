// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/bttv/liveupdates/BttvLiveUpdateMessages.hpp"

namespace {

using namespace Qt::Literals::StringLiterals;

bool tryParseChannelId(QString &channelId)
{
    if (!channelId.startsWith("twitch:"))
    {
        return false;
    }

    channelId.remove(0, 7);
    return true;
}

}

namespace chatterino {

BttvLiveUpdateEmoteUpdateAddMessage::BttvLiveUpdateEmoteUpdateAddMessage(
    const QJsonObject &json)
    : channelID(json["channel"].toString())
    , jsonEmote(json["emote"].toObject())
    , emoteName(this->jsonEmote["code"].toString())
    , emoteID(this->jsonEmote["id"].toString())
    , badChannelID_(!tryParseChannelId(this->channelID))
{
}

bool BttvLiveUpdateEmoteUpdateAddMessage::validate() const
{

    return !this->badChannelID_ && !this->channelID.isEmpty() &&
           !this->emoteID.isEmpty() && !this->emoteName.isEmpty();
}

BttvLiveUpdateEmoteRemoveMessage::BttvLiveUpdateEmoteRemoveMessage(
    const QJsonObject &json)
    : channelID(json["channel"].toString())
    , emoteID(json["emoteId"].toString())
    , badChannelID_(!tryParseChannelId(this->channelID))
{
}

bool BttvLiveUpdateEmoteRemoveMessage::validate() const
{
    return !this->badChannelID_ && !this->emoteID.isEmpty() &&
           !this->channelID.isEmpty();
}

BttvLiveUpdateUserUpdateMessage::BttvLiveUpdateUserUpdateMessage(
    const QJsonObject &json)
    : userID(json["providerId"_L1].toString())
    , badgeObject(json["badge"_L1].toObject())
{
}

bool BttvLiveUpdateUserUpdateMessage::validate() const
{
    return !this->userID.isEmpty();
}

bool BttvLiveUpdateUserUpdateMessage::hasBadge() const
{
    return !this->badgeObject.isEmpty();
}

}

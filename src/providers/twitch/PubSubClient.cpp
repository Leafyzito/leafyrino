// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/PubSubClient.hpp"

#include "common/QLogging.hpp"
#include "providers/twitch/PubSubManager.hpp"
#include "providers/twitch/PubSubMessages.hpp"

namespace chatterino {

using namespace Qt::Literals::StringLiterals;

QDebug operator<<(QDebug debug, const TopicData &data)
{
    QDebugStateSaver saver(debug);
    debug.nospace() << "TopicData(" << data.topic << ')';

    return debug;
}

PubSubClient::PubSubClient(PubSub &manager,
                           std::chrono::milliseconds heartbeatInterval)
    : BasicPubSubClient(MAX_LISTENS)
    , heartbeatInterval_(heartbeatInterval)
    , manager_(manager)
{
}

void PubSubClient::onOpen()
{
    BasicPubSubClient::onOpen();
    this->isOpen_ = true;
    this->lastHeartbeat_ = std::chrono::steady_clock::now();
}

void PubSubClient::onMessage(const QByteArray &msg)
{
    this->manager_.diag.messagesReceived++;

    auto optMessage = parsePubSubBaseMessage(msg);
    if (!optMessage)
    {
        qCDebug(chatterinoPubSub)
            << "Unable to parse incoming pubsub message" << msg;
        this->manager_.diag.messagesFailedToParse += 1;
        return;
    }

    auto message = *optMessage;

    switch (message.type)
    {
        case PubSubMessage::Type::Pong: {
            this->lastHeartbeat_.store(std::chrono::steady_clock::now());
        }
        break;

        case PubSubMessage::Type::Response: {
            this->handleResponse(message);
        }
        break;

        case PubSubMessage::Type::Message: {
            auto oMessageMessage = message.toInner<PubSubMessageMessage>();
            if (!oMessageMessage)
            {
                qCDebug(chatterinoPubSub) << "Malformed MESSAGE:" << msg;
                return;
            }

            this->handleMessageResponse(*oMessageMessage);
        }
        break;

        case PubSubMessage::Type::INVALID:
        default: {
            qCDebug(chatterinoPubSub)
                << "Unknown message type:" << message.typeString;
        }
        break;
    }
}

void PubSubClient::checkHeartbeat()
{
    if (!this->isOpen_)
    {
        return;
    }

    auto dur = std::chrono::steady_clock::now() - this->lastHeartbeat_.load();
    if (dur > this->heartbeatInterval_ * 1.5)
    {
        qCDebug(chatterinoPubSub) << "Heartbeat timed out";
        this->close();
    }

    this->sendText(R"({"type":"PING"})"_ba);
}

QByteArray PubSubClient::encodeSubscription(const Subscription &subscription)
{
    PubSubListenMessage listen({subscription.topic});
    if (!subscription.authToken.isEmpty())
    {
        listen.setToken(subscription.authToken);
    }
    this->nonces_[listen.nonce] = NonceInfo{
        .isListen = true,
    };
    return listen.toJson();
}

QByteArray PubSubClient::encodeUnsubscription(const Subscription &subscription)
{
    PubSubUnlistenMessage unlisten({subscription.topic});
    this->nonces_[unlisten.nonce] = NonceInfo{
        .isListen = false,
    };
    return unlisten.toJson();
}

void PubSubClient::handleResponse(const PubSubMessage &message)
{
    const bool failed = !message.error.isEmpty();

    if (failed)
    {
        qCDebug(chatterinoPubSub)
            << "Error" << message.error << "on nonce" << message.nonce;
    }

    if (message.nonce.isEmpty())
    {
        return;
    }

    auto nonceInfoIt = this->nonces_.find(message.nonce);
    if (nonceInfoIt == this->nonces_.end())
    {
        qCDebug(chatterinoPubSub) << "Unknown nonce:" << message.nonce;
        return;
    }

    if (nonceInfoIt->second.isListen)
    {
        if (failed)
        {
            this->manager_.diag.failedListenResponses++;
        }
        else
        {
            this->manager_.diag.listenResponses++;
        }
    }
    else
    {
        this->manager_.diag.unlistenResponses++;
    }

    this->nonces_.erase(nonceInfoIt);
}

void PubSubClient::handleMessageResponse(const PubSubMessageMessage &message)
{
    if (message.topic.startsWith("pinned-chat-updates-v1."))
    {
        auto oInnerMessage =
            message.toInner<PubSubPinnedChatUpdatesV1Message>();
        if (!oInnerMessage)
        {
            qCDebug(chatterinoPubSub)
                << "Malformed pinned-chat-updates-v1 message";
            return;
        }

        const auto &innerMessage = *oInnerMessage;
        // strip the "pinned-chat-updates-v1." prefix
        const auto channelId = message.topic.sliced(
            static_cast<qsizetype>(sizeof("pinned-chat-updates-v1.") - 1));

        // Upstream Helix pinned-message banner signals
        switch (innerMessage.type)
        {
            case PubSubPinnedChatUpdatesV1Message::Type::PinMessage:
            case PubSubPinnedChatUpdatesV1Message::Type::UpdateMessage: {
                this->manager_.pinnedChatUpdates.pinned.invoke(channelId);
            }
            break;

            case PubSubPinnedChatUpdatesV1Message::Type::UnpinMessage: {
                this->manager_.pinnedChatUpdates.unpinned.invoke(channelId);
            }
            break;

            case PubSubPinnedChatUpdatesV1Message::Type::INVALID:
            default: {
                qCDebug(chatterinoPubSub) << "Invalid pinned-chat-updates-v1 "
                                             "event type:"
                                          << innerMessage.typeString;
            }
            break;
        }

        // leafyrino payload signal (system messages / GQL pin state)
        QJsonObject payload;
        payload["type"] = innerMessage.typeString;
        payload["topic"] = message.topic;
        payload["data"] = innerMessage.data;
        this->manager_.pinnedChat.updated.invoke(payload);
        return;
    }

    if (message.topic.startsWith("community-points-channel-v1."))
    {
        auto oInnerMessage =
            message.toInner<PubSubCommunityPointsChannelV1Message>();
        if (!oInnerMessage)
        {
            qCDebug(chatterinoPubSub)
                << "Malformed community-points-channel-v1 message";
            return;
        }

        const auto &innerMessage = *oInnerMessage;

        switch (innerMessage.type)
        {
            case PubSubCommunityPointsChannelV1Message::Type::
                AutomaticRewardRedeemed:
            case PubSubCommunityPointsChannelV1Message::Type::RewardRedeemed: {
                auto redemption =
                    innerMessage.data.value("redemption").toObject();
                this->manager_.pointReward.redeemed.invoke(redemption);
            }
            break;

            case PubSubCommunityPointsChannelV1Message::Type::INVALID:
            default: {
                qCDebug(chatterinoPubSub)
                    << "Invalid point event type:" << innerMessage.typeString;
            }
            break;
        }
    }
    else if (message.topic.startsWith("predictions-channel-v1."))
    {
        auto oInnerMessage =
            message.toInner<PubSubPredictionChannelV1Message>();
        if (!oInnerMessage)
        {
            qCDebug(chatterinoPubSub)
                << "Malformed predictions-channel-v1 message";
            return;
        }

        const auto &innerMessage = *oInnerMessage;

        switch (innerMessage.type)
        {
            case PubSubPredictionChannelV1Message::Type::EventCreated:
            case PubSubPredictionChannelV1Message::Type::EventUpdated:
            case PubSubPredictionChannelV1Message::Type::EventLocked:
            case PubSubPredictionChannelV1Message::Type::EventResolved:
            case PubSubPredictionChannelV1Message::Type::EventCanceled: {
                QJsonObject payload;
                payload["type"] = innerMessage.typeString;
                payload["topic"] = message.topic;
                payload["data"] = innerMessage.data;

                this->manager_.prediction.updated.invoke(payload);
            }
            break;

            case PubSubPredictionChannelV1Message::Type::INVALID:
            default: {
                qCDebug(chatterinoPubSub) << "Invalid prediction event type:"
                                          << innerMessage.typeString;
            }
            break;
        }
    }
    else if (message.topic.startsWith("polls."))
    {
        if (message.messageObject.empty())
        {
            qCDebug(chatterinoPubSub) << "Malformed polls message";
            return;
        }

        const auto type = message.messageObject.value("type").toString();
        if (type != "POLL_CREATE" && type != "POLL_UPDATE" &&
            type != "POLL_END")
        {
            qCDebug(chatterinoPubSub) << "Invalid poll event type:" << type;
            return;
        }

        QJsonObject payload;
        payload["type"] = type;
        payload["topic"] = message.topic;
        payload["data"] = message.messageObject.value("data");

        this->manager_.poll.updated.invoke(payload);
    }
    else if (message.topic.startsWith("raid."))
    {
        if (message.messageObject.empty())
        {
            qCDebug(chatterinoPubSub) << "Malformed raid message";
            return;
        }

        const auto type = message.messageObject.value("type").toString();
        if (type != "raid_update_v2")
        {
            qCDebug(chatterinoPubSub) << "Invalid raid event type:" << type;
            return;
        }

        auto raid = message.messageObject.value("raid").toObject();
        if (raid.isEmpty())
        {
            raid = message.messageObject.value("data")
                       .toObject()
                       .value("raid")
                       .toObject();
        }
        if (raid.isEmpty())
        {
            qCDebug(chatterinoPubSub) << "Raid event missing raid payload";
            return;
        }

        QJsonObject payload;
        payload["type"] = type;
        payload["topic"] = message.topic;
        payload["raid"] = raid;

        this->manager_.raid.updated.invoke(payload);
    }
    else if (message.topic.startsWith("community-points-user-v1."))
    {
        auto oInnerMessage =
            message.toInner<PubSubCommunityPointsUserV1Message>();
        if (!oInnerMessage)
        {
            qCDebug(chatterinoPubSub)
                << "Malformed community-points-user-v1 message";
            return;
        }

        const auto &innerMessage = *oInnerMessage;

        switch (innerMessage.type)
        {
            case PubSubCommunityPointsUserV1Message::Type::PointsEarned:
            case PubSubCommunityPointsUserV1Message::Type::PointsSpent:
            case PubSubCommunityPointsUserV1Message::Type::ClaimAvailable: {
                QJsonObject payload;
                payload["type"] = innerMessage.typeString;
                payload["topic"] = message.topic;

                payload["data"] = innerMessage.data;

                this->manager_.userPoints.updated.invoke(payload);
            }
            break;

            case PubSubCommunityPointsUserV1Message::Type::INVALID:
            default: {
                qCDebug(chatterinoPubSub)
                    << "Invalid community points user event type:"
                    << innerMessage.typeString;
            }
            break;
        }
    }
    else if (message.topic.startsWith("predictions-user-v1."))
    {
        auto oInnerMessage = message.toInner<PubSubPredictionUserV1Message>();
        if (!oInnerMessage)
        {
            qCDebug(chatterinoPubSub)
                << "Malformed predictions-user-v1 message";
            return;
        }

        const auto &innerMessage = *oInnerMessage;

        switch (innerMessage.type)
        {
            case PubSubPredictionUserV1Message::Type::PredictionMade:
            case PubSubPredictionUserV1Message::Type::PredictionResult: {
                QJsonObject payload;
                payload["type"] = innerMessage.typeString;
                payload["topic"] = message.topic;
                payload["data"] = innerMessage.data;

                this->manager_.prediction.userResult.invoke(payload);
            }
            break;

            case PubSubPredictionUserV1Message::Type::INVALID:
            default: {
                qCDebug(chatterinoPubSub)
                    << "Invalid prediction user event type:"
                    << innerMessage.typeString;
            }
            break;
        }
    }
    else if (message.topic.startsWith("chatrooms-user-v1."))
    {
        if (message.messageObject.empty())
        {
            qCDebug(chatterinoPubSub) << "Malformed chatrooms-user-v1 message";
            return;
        }

        const auto type = message.messageObject.value("type").toString();
        const auto data = message.messageObject.value("data").toObject();
        const auto action = data.value("action").toString();
        if (type != "user_moderation_action" ||
            (action != "warn" && action != "acknowledge_warning"))
        {
            qCDebug(chatterinoPubSub)
                << "Invalid chat warning event type:" << type
                << "action:" << action;
            return;
        }

        QJsonObject payload;
        payload["type"] = type;
        payload["topic"] = message.topic;
        payload["data"] = data;

        this->manager_.chatWarning.updated.invoke(payload);
    }
}

}  // namespace chatterino

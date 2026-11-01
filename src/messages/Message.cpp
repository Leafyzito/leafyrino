#include "messages/Message.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "controllers/highlights/HighlightMatch.hpp"
#include "messages/MessageThread.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "singletons/Settings.hpp"
#include "util/DebugCount.hpp"
#include "util/QMagicEnum.hpp"
#include "widgets/helper/ScrollbarHighlight.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>

namespace chatterino {

using namespace literals;

Message::Message()
{
    DebugCount::increase(DebugObject::Message);
}

Message::~Message()
{
    DebugCount::decrease(DebugObject::Message);
}

ScrollbarHighlight Message::getScrollBarHighlight() const
{
    if (this->flags.has(MessageFlag::Highlighted))
    {
        return {
            this->highlightColor,
        };
    }

    if (this->flags.has(MessageFlag::ChatWarning))
    {
        return {
            std::make_shared<QColor>(64, 64, 64, 127),
        };
    }

    return {};
}

bool Message::usesTwitchGigantifyPresentation() const
{
    return this->flags.has(MessageFlag::GigantifiedEmote) &&
           getSettings()->enableGigantifyEmotes;
}

std::shared_ptr<Message> Message::clone() const
{
    auto cloned = std::make_shared<Message>();
    cloned->flags = this->flags;
    cloned->id = this->id;
    cloned->searchText = this->searchText;
    cloned->messageText = this->messageText;
    cloned->loginName = this->loginName;
    cloned->displayName = this->displayName;
    cloned->localizedName = this->localizedName;
    cloned->userID = this->userID;
    cloned->timeoutUser = this->timeoutUser;
    cloned->channelName = this->channelName;
    cloned->sharedChatSourceId = this->sharedChatSourceId;
    cloned->usernameColor = this->usernameColor;
    cloned->serverReceivedTime = this->serverReceivedTime;
    cloned->twitchBadges = this->twitchBadges;
    cloned->twitchBadgeInfos = this->twitchBadgeInfos;
    cloned->externalBadges = this->externalBadges;
    cloned->highlightColor = this->highlightColor;
    cloned->highlightMatches = this->highlightMatches;
    cloned->autoModReview = this->autoModReview;
    cloned->replyThread = this->replyThread;
    cloned->replyParent = this->replyParent;
    cloned->translatedFrom = this->translatedFrom;
    cloned->count = this->count;
    cloned->reward = this->reward;
    cloned->bits = this->bits;
    cloned->announcementColor = this->announcementColor;
    cloned->platform = this->platform;
    cloned->clientDetection = this->clientDetection;
    std::ranges::transform(this->elements, std::back_inserter(cloned->elements),
                           [](const auto &element) {
                               return element->clone();
                           });
    return cloned;
}

QJsonObject Message::toJson() const
{
    QJsonObject msg{
        {"flags"_L1, qmagicenum::enumFlagsName(this->flags.value())},
        {"id"_L1, this->id},
        {"searchText"_L1, this->searchText},
        {"messageText"_L1, this->messageText},
        {"loginName"_L1, this->loginName},
        {"displayName"_L1, this->displayName},
        {"localizedName"_L1, this->localizedName},
        {"userID"_L1, this->userID},
        {"timeoutUser"_L1, this->timeoutUser},
        {"channelName"_L1, this->channelName},
        {"usernameColor"_L1, this->usernameColor.name(QColor::HexArgb)},
        {"count"_L1, static_cast<qint64>(this->count)},
        {"serverReceivedTime"_L1,
         this->serverReceivedTime.toString(Qt::ISODate)},
        {"frozen"_L1, this->frozen},
    };

    QJsonArray twitchBadges;
    for (const auto &badge : this->twitchBadges)
    {
        twitchBadges.append(badge.key_);
    }
    msg["twitchBadges"_L1] = twitchBadges;

    QJsonObject twitchBadgeInfos;
    for (const auto &[key, value] : this->twitchBadgeInfos)
    {
        twitchBadgeInfos.insert(key, value);
    }
    msg["twitchBadgeInfos"_L1] = twitchBadgeInfos;

    msg["externalBadges"_L1] = QJsonArray::fromStringList(this->externalBadges);

    if (this->highlightColor)
    {
        msg["highlightColor"_L1] = this->highlightColor->name(QColor::HexArgb);
    }

    if (this->highlightMatches && !this->highlightMatches->empty())
    {
        QJsonArray matches;
        for (const auto &match : *this->highlightMatches)
        {
            matches.append(QJsonObject{
                {"start"_L1, static_cast<qint64>(match.start)},
                {"length"_L1, static_cast<qint64>(match.length)},
                {"pattern"_L1, match.pattern},
                {"ruleName"_L1, match.ruleName},
            });
        }
        msg["highlightMatches"_L1] = matches;
    }

    if (this->replyThread)
    {
        msg["replyThread"_L1] = this->replyThread->toJson();
    }

    if (this->replyParent)
    {
        msg["replyParent"_L1] = this->replyParent->id;
    }

    if (this->reward)
    {
        msg["reward"_L1] = this->reward->toJson();
    }

    if (this->flags.has(MessageFlag::Announcement))
    {
        msg["announcementColor"_L1] =
            qmagicenum::enumNameString(this->announcementColor);
    }

    if (!getApp()->isTest())
    {
        msg["parseTime"_L1] = this->parseTime.toString(Qt::ISODate);
    }

    QJsonArray elements;
    for (const auto &element : this->elements)
    {
        elements.append(element->toJson());
    }
    msg["elements"_L1] = elements;

    if (this->platform != MessagePlatform::AnyOrTwitch)
    {
        msg["platform"_L1] = qmagicenum::enumNameString(this->platform);
    }

    return msg;
}

QString Message::clientDetectionStatusToString(ClientDetectionStatus status)
{
    switch (status)
    {
        case ClientDetectionStatus::Web:
            return QStringLiteral("Web");
        case ClientDetectionStatus::Android:
            return QStringLiteral("Android");
        case ClientDetectionStatus::IOS:
            return QStringLiteral("iOS");
        case ClientDetectionStatus::MobileNew:
            return QStringLiteral("Mobile");
        case ClientDetectionStatus::Abnormal:
            return QStringLiteral("Abnormal");
        case ClientDetectionStatus::Unknown:
        default:
            return QStringLiteral("Unknown");
    }
}

Message::ReplyStatus Message::isReplyable() const
{
    if (this->loginName.isEmpty())
    {
        return ReplyStatus::NotReplyable;
    }

    constexpr int oneDayInSeconds = 24 * 60 * 60;
    bool messageReplyable = true;
    if (this->flags.hasAny({MessageFlag::System, MessageFlag::Subscription,
                            MessageFlag::Timeout, MessageFlag::Whisper,
                            MessageFlag::ModerationAction,
                            MessageFlag::InvalidReplyTarget}) ||
        this->serverReceivedTime.secsTo(QDateTime::currentDateTime()) >
            oneDayInSeconds)
    {
        messageReplyable = false;
    }

    if (this->replyThread != nullptr)
    {
        if (const auto &rootPtr = this->replyThread->root(); rootPtr != nullptr)
        {
            assert(this != rootPtr.get());
            if (rootPtr->isReplyable() == ReplyStatus::NotReplyable)
            {
                return ReplyStatus::NotReplyableDueToThread;
            }

            return messageReplyable ? ReplyStatus::ReplyableWithThread
                                    : ReplyStatus::NotReplyableWithThread;
        }
    }

    return messageReplyable ? ReplyStatus::Replyable
                            : ReplyStatus::NotReplyable;
}

}  // namespace chatterino

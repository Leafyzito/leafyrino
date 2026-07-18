#pragma once

#include "messages/MessageFlag.hpp"
#include "providers/twitch/api/HelixEnums.hpp"
#include "providers/twitch/ChannelPointReward.hpp"
#include "util/DebugCount.hpp"
#include "util/QStringHash.hpp"

#include <QColor>
#include <QTime>

#include <cinttypes>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

class QJsonObject;

namespace chatterino {
class MessageElement;
class MessageThread;
class TwitchBadge;
class ScrollbarHighlight;

enum class MessagePlatform : uint8_t {
    AnyOrTwitch,
    Kick,
};

struct Message;
using MessagePtr = std::shared_ptr<const Message>;
using MessagePtrMut = std::shared_ptr<Message>;
struct Message {
    Message();
    ~Message();

    Message(const Message &) = delete;
    Message &operator=(const Message &) = delete;

    Message(Message &&) = delete;
    Message &operator=(Message &&) = delete;

    mutable MessageFlags flags;
    QTime parseTime;
    QString id;
    QString searchText;
    QString messageText;

    QString loginName;
    QString displayName;
    QString localizedName;
    QString userID;
    QString timeoutUser;
    QString channelName;
    QColor usernameColor;
    QDateTime serverReceivedTime;

    std::vector<TwitchBadge> twitchBadges;

    std::unordered_map<QString, QString> twitchBadgeInfos;

    QStringList externalBadges;

    std::shared_ptr<QColor> highlightColor;

    std::shared_ptr<MessageThread> replyThread;
    MessagePtr replyParent;
    MessagePtr translatedFrom;
    enum class ReplyStatus : std::uint8_t {

        NotReplyable,

        Replyable,

        ReplyableWithThread,

        NotReplyableWithThread,

        NotReplyableDueToThread,
    };
    ReplyStatus isReplyable() const;
    enum class ClientDetectionStatus : std::uint8_t {
        // No client-nonce
        Unknown = 0,
        // [0-9a-f]{32}, not really a uuid
        Web,
        Webchat = Web,
        // UUID4 (standard) lowercase
        Android,
        // UUID4 (standard) uppercase
        IOS,
        // Does not match any of the known clients
        Abnormal,
    };
    static QString clientDetectionStatusToString(ClientDetectionStatus status);

    uint32_t count = 1;

    mutable bool frozen = false;

    MessagePlatform platform = MessagePlatform::AnyOrTwitch;

    std::vector<std::unique_ptr<MessageElement>> elements;
    ClientDetectionStatus clientDetection = ClientDetectionStatus::Unknown;

    ScrollbarHighlight getScrollBarHighlight() const;

    std::shared_ptr<ChannelPointReward> reward = nullptr;

    uint32_t bits{0};

    HelixAnnouncementColor announcementColor{HelixAnnouncementColor::Primary};

    std::shared_ptr<Message> clone() const;

    QJsonObject toJson() const;

    void freeze() const
    {
        this->frozen = true;
    }
};

}  // namespace chatterino

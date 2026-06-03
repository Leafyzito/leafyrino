#pragma once

#include "common/Aliases.hpp"
#include "common/Outcome.hpp"
#include "messages/MessageColor.hpp"
#include "messages/MessageFlag.hpp"

#include <IrcMessage>
#include <QRegularExpression>
#include <QString>
#include <QTime>
#include <QUrl>
#include <QVariant>

#include <ctime>
#include <memory>
#include <utility>

namespace chatterino {

struct Message;
using MessagePtr = std::shared_ptr<const Message>;
using MessagePtrMut = std::shared_ptr<Message>;

class MessageElement;
class TextElement;
struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

class Channel;
class TwitchChannel;
class MessageThread;
class IgnorePhrase;
struct HelixVip;
using HelixModerator = HelixVip;
struct ChannelPointReward;
struct TwitchEmoteOccurrence;
class ChannelChatters;

namespace linkparser {
struct Parsed;
}

struct SystemMessageTag {
};
struct TimeoutMessageTag {
};
struct LiveUpdatesUpdateEmoteMessageTag {
};
struct LiveUpdatesRemoveEmoteMessageTag {
};
struct LiveUpdatesAddEmoteMessageTag {
};
struct LiveUpdatesUpdateEmoteSetMessageTag {
};
struct ImageUploaderResultTag {
};

const SystemMessageTag systemMessage{};
const TimeoutMessageTag timeoutMessage{};
const LiveUpdatesUpdateEmoteMessageTag liveUpdatesUpdateEmoteMessage{};
const LiveUpdatesRemoveEmoteMessageTag liveUpdatesRemoveEmoteMessage{};
const LiveUpdatesAddEmoteMessageTag liveUpdatesAddEmoteMessage{};
const LiveUpdatesUpdateEmoteSetMessageTag liveUpdatesUpdateEmoteSetMessage{};

const ImageUploaderResultTag imageUploaderResultMessage{};

MessagePtr makeSystemMessage(const QString &text);
MessagePtr makeSystemMessage(const QString &text, const QTime &time);

struct MessageParseArgs {
    bool disablePingSounds = false;
    bool isReceivedWhisper = false;
    bool isSentWhisper = false;
    bool trimSubscriberUsername = false;
    bool isStaffOrBroadcaster = false;
    bool isSubscriptionMessage = false;
    bool allowIgnore = true;
    bool isAction = false;
    QString channelPointRewardId = "";
};

struct HighlightAlert {
    QUrl customSound;
    bool playSound = false;
    bool windowAlert = false;
};
class MessageBuilder
{
public:

    MessageBuilder();

    MessageBuilder(SystemMessageTag, const QString &text,
                   const QTime &time = QTime::currentTime());
    MessageBuilder(TimeoutMessageTag, const QString &timeoutUser,
                   const QString &sourceUser, const QString &channel,
                   const QString &systemMessageText, uint32_t times,
                   const QDateTime &time);
    MessageBuilder(TimeoutMessageTag, const QString &username,
                   const QString &durationInSeconds, bool multipleTimes,
                   const QDateTime &time);

    MessageBuilder(LiveUpdatesAddEmoteMessageTag, const QString &platform,
                   const QString &actor,
                   const std::vector<QString> &emoteNames);
    MessageBuilder(LiveUpdatesRemoveEmoteMessageTag, const QString &platform,
                   const QString &actor,
                   const std::vector<QString> &emoteNames);
    MessageBuilder(LiveUpdatesUpdateEmoteMessageTag, const QString &platform,
                   const QString &actor, const QString &emoteName,
                   const QString &oldEmoteName);
    MessageBuilder(LiveUpdatesUpdateEmoteSetMessageTag, const QString &platform,
                   const QString &actor, const QString &emoteSetName);

    MessageBuilder(ImageUploaderResultTag, const QString &imageLink,
                   const QString &deletionLink, size_t imagesStillQueued = 0,
                   size_t secondsLeft = 0);

    MessageBuilder(const MessageBuilder &) = delete;
    MessageBuilder(MessageBuilder &&) = delete;
    MessageBuilder &operator=(const MessageBuilder &) = delete;
    MessageBuilder &operator=(MessageBuilder &&) = delete;

    ~MessageBuilder() = default;

    Message *operator->();
    Message &message();
    MessagePtrMut release();
    std::weak_ptr<const Message> weakOf();

    void append(std::unique_ptr<MessageElement> element);
    void addLink(const linkparser::Parsed &parsedLink, QStringView source);

    template <typename T, typename... Args>
    T *emplace(Args &&...args)
    {
        static_assert(std::is_base_of_v<MessageElement, T>,
                      "T must extend MessageElement");

        auto unique = std::make_unique<T>(std::forward<Args>(args)...);
        auto pointer = unique.get();
        this->append(std::move(unique));
        return pointer;
    }

    void appendOrEmplaceText(const QString &text, MessageColor color);
    void appendOrEmplaceSystemTextAndUpdate(const QString &text,
                                            QString &toUpdate);

    TextElement *emplaceSystemTextAndUpdate(const QString &text,
                                            QString &toUpdate);

    void addWordFromUserMessage(QStringView string,
                                ChannelChatters *chatters = nullptr);

    void appendEmote(const EmotePtr &emote);

    MessageColor textColor() const;

    static void triggerHighlights(const Channel *channel,
                                  const HighlightAlert &alert);
    static void triggerHighlights(const Channel *channel,
                                  const MessagePtr &message,
                                  const HighlightAlert &alert);

    void appendChannelPointRewardMessage(const ChannelPointReward &reward,
                                         bool isMod, bool isBroadcaster);

    static MessagePtr makeChannelPointRewardMessage(
        const ChannelPointReward &reward, bool isMod, bool isBroadcaster);

    static MessagePtr makeLiveMessage(const QString &channelName,
                                      const QString &channelID,
                                      const QString &title,
                                      MessageFlags extraFlags = {});

    static MessagePtr makeOfflineSystemMessage(const QString &channelName,
                                               const QString &channelID);
    static MessagePtr makeHostingSystemMessage(const QString &channelName,
                                               bool hostOn);
    static MessagePtr makeDeletionMessageFromIRC(
        const MessagePtr &originalMessage);
    static MessagePtr makeListOfUsersMessage(QString prefix, QStringList users,
                                             Channel *channel,
                                             MessageFlags extraFlags = {});
    static MessagePtr makeListOfUsersMessage(
        QString prefix, const std::vector<HelixModerator> &users,
        Channel *channel, MessageFlags extraFlags = {});

    static MessagePtr buildHypeChatMessage(Communi::IrcPrivateMessage *message);

    static std::pair<MessagePtrMut, HighlightAlert> makeIrcMessage(
        Channel *channel, const Communi::IrcMessage *ircMessage,
        const MessageParseArgs &args, QString content,
        QString::size_type messageOffset,
        const std::shared_ptr<MessageThread> &thread = {},
        const MessagePtr &parent = {});

    static MessagePtrMut makeSystemMessageWithUser(
        const QString &text, const QString &loginName,
        const QString &displayName, const MessageColor &userColor,
        const QTime &time);

    static MessagePtrMut makeSubgiftMessage(const QString &text,
                                            const QVariantMap &tags,
                                            const QTime &time,
                                            TwitchChannel *channel);

    static MessagePtrMut makeMissingScopesMessage(const QString &missingScopes);

    static MessagePtrMut makeClearChatMessage(const QDateTime &now,
                                              const QString &actor,
                                              uint32_t count = 1);

private:
    struct TextState {
        TwitchChannel *twitchChannel = nullptr;
        QString userID;
        bool hasBits = false;
        bool bitsStacked = false;
        int bitsLeft = 0;
    };
    void addEmoji(const EmotePtr &emote);
    void addTextOrEmote(TextState &state, QString string);

    Outcome tryAppendCheermote(TextState &state, const QString &string);
    Outcome tryAppendEmote(TwitchChannel *twitchChannel, const QString &userID,
                           const EmoteName &name);

    bool isEmpty() const;
    MessageElement &back();
    std::unique_ptr<MessageElement> releaseBack();

    void parse();
    void parseUsernameColor(const QVariantMap &tags, const QString &userID);
    void parseUsername(const Communi::IrcMessage *ircMessage,
                       TwitchChannel *twitchChannel,
                       bool trimSubscriberUsername);
    void parseMessageID(const QVariantMap &tags);

    static QString parseRoomID(const QVariantMap &tags,
                               TwitchChannel *twitchChannel);

    TwitchChannel *parseSharedChatInfo(const QVariantMap &tags,
                                       TwitchChannel *twitchChannel);

    void parseThread(const QString &messageContent, const QVariantMap &tags,
                     const Channel *channel,
                     const std::shared_ptr<MessageThread> &thread,
                     const MessagePtr &parent);

    HighlightAlert parseHighlights(const QVariantMap &tags,
                                   const QString &originalMessage,
                                   const MessageParseArgs &args);

    void appendChannelName(const Channel *channel);
    void appendUsername(const QVariantMap &tags, const MessageParseArgs &args);

    void addWords(const QStringList &words,
                  const std::vector<TwitchEmoteOccurrence> &twitchEmotes,
                  TextState &state);

    void appendTwitchBadges(const QVariantMap &tags,
                            TwitchChannel *twitchChannel);
    void appendChatterinoBadges(const QString &userID);
    void appendFfzBadges(TwitchChannel *twitchChannel, const QString &userID);
    void appendBttvBadges(const QString &userID);
    void appendSeventvBadges(const QString &userID);
    void appendHomiesBadges(const QString &userID);
    void appendMoltorinoBadges(const QString &userID);

    [[nodiscard]] static bool isIgnored(const QString &originalMessage,
                                        const QString &userID,
                                        const Channel *channel);

    std::shared_ptr<Message> message_;
    MessageColor textColor_ = MessageColor::Text;

    QColor usernameColor_ = {153, 153, 153};
};

}

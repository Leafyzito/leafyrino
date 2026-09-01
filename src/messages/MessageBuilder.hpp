#pragma once

#include "common/Aliases.hpp"
#include "common/Outcome.hpp"
#include "messages/ast/Parser.hpp"
#include "messages/MessageColor.hpp"
#include "messages/MessageElement.hpp"
#include "messages/MessageFlag.hpp"
#include "singletons/Fonts.hpp"

#include <IrcMessage>
#include <IrcTagsRef>
#include <QColor>
#include <QRegularExpression>
#include <QString>
#include <QTime>
#include <QUrl>
#include <QVariant>

#include <ctime>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

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
class TwitchBadge;
class ChannelChatters;
class MessageThread;
class IgnorePhrase;
struct HelixVip;
using HelixModerator = HelixVip;
struct ChannelPointReward;
struct TwitchEmoteOccurrence;

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

struct BadgePreviewFallback {
    QString setID;
    QString version;
    QString title;
    QString image1x;
    QString image2x;
    QString image4x;
};

struct LiveUpdateEmote {
    QString name;
    QString url;
    EmotePtr emote;

    LiveUpdateEmote() = default;

    explicit LiveUpdateEmote(EmotePtr emote);
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
                   const std::vector<LiveUpdateEmote> &emotes);
    MessageBuilder(LiveUpdatesRemoveEmoteMessageTag, const QString &platform,
                   const QString &actor,
                   const std::vector<LiveUpdateEmote> &emotes);
    MessageBuilder(LiveUpdatesUpdateEmoteMessageTag, const QString &platform,
                   const QString &actor, const QString &emoteName,
                   const QString &oldEmoteName, const EmotePtr &emote);
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
    void addLink(const linkparser::Parsed &parsedLink, QStringView source,
                 const QString &textOverride = QString());

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

    MessageElement *appendOrEmplaceText(
        const QString &text, MessageColor color,
        MessageElementFlags messageFlags = MessageElementFlag::Text,
        FontStyle style = FontStyle::ChatMedium);
    void appendOrEmplaceSystemTextAndUpdate(const QString &text,
                                            QString &toUpdate);

    TextElement *emplaceSystemTextAndUpdate(const QString &text,
                                            QString &toUpdate);

    void addWordFromUserMessage(QStringView string,
                                ChannelChatters *chatters = nullptr,
                                FontStyle style = FontStyle::ChatMedium);

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

    /// @brief Builds a message out of an `ircMessage`.
    ///
    /// Building a message won't cause highlights to be triggered. They will
    /// only be parsed. To trigger highlights (play sound etc.), use
    /// triggerHighlights().
    ///
    /// @param channel The channel this message was sent to. Must not be
    ///                `nullptr`.
    /// @param ircMessage The original message. This can be any message
    ///                   (PRIVMSG, USERNOTICE, etc.). Its content is not
    ///                   accessed through this parameter but through `content`,
    ///                   as the content might be inside a tag (e.g. gifts in a
    ///                   USERNOTICE).
    /// @param args Arguments from parsing a chat message.
    /// @param content The message text. This isn't always the entire text. In
    ///                replies, the leading mention can be cut off.
    ///                See `messageOffset`.
    /// @param messageOffset Starting offset to be used on index-based
    ///                      operations on `content` such as parsing emotes.
    ///                      For example:
    ///                         ircMessage = "@hi there"
    ///                         content = "there"
    ///                         messageOffset_ = 4
    ///                      The index 6 would resolve to 6 - 4 = 2 => 'e'
    /// @param thread The reply thread this message is part of. If there's no
    ///               thread, this is an empty `shared_ptr`.
    /// @param parent The direct parent this message is replying to. This does
    ///               not need to be the `thread`s root. If this message isn't
    ///               replying to anything, this is an empty `shared_ptr`.
    ///
    /// @returns The built message and a highlight result. If the message is
    ///          ignored (e.g. from a blocked user), then the returned pointer
    ///          will be en empty `shared_ptr`.
    static std::pair<MessagePtrMut, HighlightAlert> makeIrcMessage(
        Channel *channel, const Communi::IrcMessage *ircMessage,
        const MessageParseArgs &args, QString content,
        QString::size_type messageOffset,
        const std::shared_ptr<MessageThread> &thread = {},
        const MessagePtr &parent = {});

    static MessagePtrMut makeSystemMessageWithUser(
        const QString &text, const QString &loginName,
        const QString &displayName, const MessageColor &userColor,
        const QTime &time, const Communi::IrcMessage &ircMessage,
        TwitchChannel *channel);

    static MessagePtrMut makeSubgiftMessage(Communi::TagsRef tags,
                                            const QTime &time,
                                            TwitchChannel *channel);

    static MessagePtrMut makeMissingScopesMessage(const QString &missingScopes);

    static MessagePtrMut makeClearChatMessage(const QDateTime &now,
                                              const QString &actor,
                                              uint32_t count = 1);

    static MessagePtr makeSelfBadgePreviewMessage(
        TwitchChannel *channel, const QString &userId, const QString &loginName,
        const QString &displayName, const std::optional<QColor> &userColor,
        const std::vector<TwitchBadge> &twitchBadges,
        const std::vector<BadgePreviewFallback> &badgeFallbacks);

private:
    struct TextState {
        TwitchChannel *twitchChannel = nullptr;
        QString userID;  // 7TV: used for personal emotes
        bool hasBits = false;
        bool bitsStacked = false;
        int bitsLeft = 0;
    };
    void addEmoji(const EmotePtr &emote);
    void addTextOrEmote(TextState &state, QString string,
                        FontStyle style = FontStyle::ChatMedium);

    Outcome tryAppendCheermote(TextState &state, const QString &string);
    Outcome tryAppendEmote(TwitchChannel *twitchChannel, const QString &userID,
                           EmoteNameView name);

    bool isEmpty() const;
    MessageElement &back();
    std::unique_ptr<MessageElement> releaseBack();

    void parse();
    void parseUsernameColor(Communi::TagsRef tags, const QString &userID);
    void parseUsername(const Communi::IrcMessage *ircMessage,
                       TwitchChannel *twitchChannel,
                       bool trimSubscriberUsername);
    void parseMessageID(Communi::TagsRef tags);
    /// Parses most of the message flags based on the given tags
    void parseMessageTags(Communi::TagsRef tags, TwitchChannel *channel,
                          bool hasContent);

    /// Parses the room-ID this message was received in
    ///
    /// @returns The room-ID
    static QString parseRoomID(Communi::TagsRef tags,
                               TwitchChannel *twitchChannel);

    /// Parses the shared-chat information from this message.
    ///
    /// @param tags The tags of the received message
    /// @param twitchChannel The channel this message was received in
    /// @returns The source channel - the channel this message originated from.
    ///          If there's no channel currently open, @a twitchChannel is
    ///          returned.
    TwitchChannel *parseSharedChatInfo(Communi::TagsRef tags,
                                       TwitchChannel *twitchChannel);

    // Parse & build thread information into the message
    // Will read information from thread_ or from IRC tags
    void parseThread(const QString &messageContent, Communi::TagsRef tags,
                     const Channel *channel,
                     const std::shared_ptr<MessageThread> &thread,
                     const MessagePtr &parent);
    // parseHighlights only updates the visual state of the message, but leaves the playing of alerts and sounds to the triggerHighlights function
    HighlightAlert parseHighlights(Communi::TagsRef tags,
                                   const QString &originalMessage,
                                   const MessageParseArgs &args);

    void appendChannelName(const Channel *channel);
    void appendUsername(Communi::TagsRef tags, const MessageParseArgs &args);

    void addWordsFromAstNodes(
        const QVector<ast::ASTNode> &nodes,
        const std::vector<TwitchEmoteOccurrence> &twitchEmotes,
        TextState &state, FontStyle style = FontStyle::ChatMedium);
    void addWords(const QStringList &words,
                  const std::vector<TwitchEmoteOccurrence> &twitchEmotes,
                  TextState &state, FontStyle style = FontStyle::ChatMedium);

    void appendTwitchBadges(Communi::TagsRef tags,
                            TwitchChannel *twitchChannel);
    void appendChatterinoBadges(const QString &userID);
    void appendFfzBadges(TwitchChannel *twitchChannel, const QString &userID);
    void appendBttvBadges(const QString &userID);
    void appendSeventvBadges(const QString &userID);
    void appendHomiesBadges(const QString &userID);
    void appendFolhinhaBadges(const QString &userID);
    void appendFfzApBadges(const QString &userID);
    void appendDankChatBadges(const QString &userID);
    void appendChatsenBadges(const QString &userID);
    void appendMoltorinoBadges(const QString &userID);

    [[nodiscard]] static bool isIgnored(const QString &originalMessage,
                                        const QString &userID,
                                        const Channel *channel);

    std::shared_ptr<Message> message_;
    MessageColor textColor_ = MessageColor::Text;

    QColor usernameColor_ = {153, 153, 153};
};

}  // namespace chatterino

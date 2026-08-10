#include "providers/youtube/YouTubeMessageBuilder.hpp"

#include "Application.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/Link.hpp"
#include "messages/Message.hpp"
#include "messages/MessageColor.hpp"
#include "messages/MessageElement.hpp"
#include "providers/emoji/Emojis.hpp"
#include "providers/youtube/YouTubeApi.hpp"
#include "providers/youtube/YouTubeBadges.hpp"
#include "providers/youtube/YouTubeChannel.hpp"
#include "singletons/Settings.hpp"
#include "util/Helpers.hpp"
#include "util/Variant.hpp"

#include <QDateTime>

using namespace Qt::Literals::StringLiterals;

namespace {

using namespace chatterino;

constexpr uint16_t CUSTOM_EMOJI_BASE_SIZE = 28;
constexpr uint16_t STICKER_BASE_SIZE = 40;

constexpr int SUPERCHAT_HIGHLIGHT_ALPHA = 110;
constexpr int MEMBERSHIP_HIGHLIGHT_ALPHA = 96;

EmotePtr makeEmojiEmote(const YouTubeMessageRun &run)
{
    auto name = run.text.isEmpty() ? u"emoji"_s : run.text;
    return std::make_shared<const Emote>(Emote{
        .name = {name},
        .images = ImageSet{Image::fromAutoscaledUrl({run.imageUrl},
                                                    CUSTOM_EMOJI_BASE_SIZE)},
        .tooltip = Tooltip{name},
    });
}

EmotePtr makeStickerEmote(const QString &url)
{
    return std::make_shared<const Emote>(Emote{
        .name = {u"sticker"_s},
        .images = ImageSet{Image::fromAutoscaledUrl({url}, STICKER_BASE_SIZE)},
        .tooltip = Tooltip{u"Super Sticker"_s},
    });
}

MessageColor youtubeUsernameColor(const std::vector<YouTubeAuthorBadge> &badges,
                                  bool colorByRole, QColor &storedColor)
{
    if (colorByRole)
    {
        bool owner = false;
        bool moderator = false;
        bool member = false;
        for (const auto &badge : badges)
        {
            switch (badge.kind)
            {
                case YouTubeAuthorBadgeKind::Owner:
                    owner = true;
                    break;
                case YouTubeAuthorBadgeKind::Moderator:
                    moderator = true;
                    break;
                case YouTubeAuthorBadgeKind::Member:
                    member = true;
                    break;
                case YouTubeAuthorBadgeKind::Verified:
                    break;
            }
        }
        if (owner)
        {
            storedColor = QColor(255, 213, 0);  // gold
            return {storedColor};
        }
        if (moderator)
        {
            storedColor = QColor(94, 132, 241);  // YouTube mod blue
            return {storedColor};
        }
        if (member)
        {
            storedColor = QColor(42, 166, 64);  // membership green
            return {storedColor};
        }
    }
    storedColor = QColor(153, 153, 153);
    return {MessageColor::System};
}

void appendBadges(YouTubeMessageBuilder &builder,
                  const std::vector<YouTubeAuthorBadge> &badges)
{
    for (const auto &badge : badges)
    {
        EmotePtr emote;
        auto flag = MessageElementFlag::BadgeChannelAuthority;
        switch (badge.kind)
        {
            case YouTubeAuthorBadgeKind::Member:
                emote = YouTubeBadges::member(badge.imageUrl, badge.tooltip);
                flag = MessageElementFlag::BadgeSubscription;
                break;
            case YouTubeAuthorBadgeKind::Moderator:
                emote = YouTubeBadges::moderator();
                flag = MessageElementFlag::BadgeChannelAuthority;
                break;
            case YouTubeAuthorBadgeKind::Owner:
                emote = YouTubeBadges::owner();
                flag = MessageElementFlag::BadgeChannelAuthority;
                break;
            case YouTubeAuthorBadgeKind::Verified:
                emote = YouTubeBadges::verified();
                flag = MessageElementFlag::BadgeGlobalAuthority;
                break;
        }
        if (emote)
        {
            builder.emplace<BadgeElement>(emote, flag);
        }
    }
}

void appendMessageRuns(YouTubeMessageBuilder &builder,
                       const std::vector<YouTubeMessageRun> &runs,
                       QString &messageText)
{
    for (const auto &run : runs)
    {
        if (run.kind == YouTubeRunKind::Emoji && !run.imageUrl.isEmpty())
        {
            builder.appendEmote(makeEmojiEmote(run));
            if (!messageText.isEmpty())
            {
                messageText += u' ';
            }
            messageText += run.text;
            continue;
        }

        const auto words = run.text.split(u' ', Qt::SkipEmptyParts);
        for (const auto &word : words)
        {
            for (const auto &part :
                 getApp()->getEmotes()->getEmojis()->parse(word))
            {
                std::visit(variant::Overloaded{
                               [&](const EmotePtr &emote) {
                                   builder.emplace<EmoteElement>(
                                       emote, MessageElementFlag::EmojiAll);
                               },
                               [&](const QStringView &text) {
                                   builder.addWordFromUserMessage(text);
                               },
                           },
                           part);
            }
            if (!messageText.isEmpty())
            {
                messageText += u' ';
            }
            messageText += word;
        }
    }
}

}  // namespace

namespace chatterino {

YouTubeMessageBuilder::YouTubeMessageBuilder(YouTubeChannel *channel,
                                             const QDateTime &time)
    : channel_(channel)
{
    this->message().platform = MessagePlatform::YouTube;
    this->message().serverReceivedTime = time;
}

MessagePtrMut YouTubeMessageBuilder::makeChatMessage(
    YouTubeChannel *channel, const YouTubeChatItem &item)
{
    auto time = item.timestampUsec > 0
                    ? QDateTime::fromMSecsSinceEpoch(item.timestampUsec / 1000)
                    : QDateTime::currentDateTime();

    YouTubeMessageBuilder builder(channel, time);
    builder->platform = MessagePlatform::YouTube;
    builder->id = u"yt-"_s % item.id;
    builder->channelName = channel->getName();
    builder->parseTime = QTime::currentTime();
    builder->displayName = item.authorName;
    builder->loginName = item.authorChannelId;
    YouTubeChannel::rememberAuthorPhoto(item.authorChannelId, item.authorPhoto);

    QColor storedColor;
    auto nameColor = youtubeUsernameColor(
        item.authorBadges, getSettings()->colorYouTubeUsernamesByRole,
        storedColor);
    builder->usernameColor = storedColor;

    switch (item.kind)
    {
        case YouTubeChatItemKind::SuperChat:
        case YouTubeChatItemKind::SuperSticker:
            builder.buildSuperChat(item, time, storedColor);
            return builder.release();

        case YouTubeChatItemKind::Gift:
        case YouTubeChatItemKind::Membership:
            builder.buildMembership(item, time, storedColor);
            return builder.release();

        case YouTubeChatItemKind::Unsupported:
            return {};

        case YouTubeChatItemKind::Text:
            break;
    }

    builder.appendChannelName();
    builder.emplace<TimestampElement>(time.time());
    appendBadges(builder, item.authorBadges);
    builder.appendUsername(item.authorName, nameColor);

    QString messageText;
    appendMessageRuns(builder, item.runs, messageText);
    builder->messageText = messageText;
    builder->searchText = item.authorName % u": " % messageText;

    return builder.release();
}

void YouTubeMessageBuilder::buildSuperChat(const YouTubeChatItem &item,
                                           const QDateTime &time,
                                           const QColor &userColor)
{
    if (getSettings()->highlightYouTubeSuperChats)
    {
        QColor bg = item.bodyBackgroundColor != 0
                        ? QColor::fromRgba(item.bodyBackgroundColor)
                        : QColor(21, 101, 192);  // default YouTube blue tier
        bg.setAlpha(SUPERCHAT_HIGHLIGHT_ALPHA);
        this->message().flags.set(MessageFlag::Highlighted);
        this->message().highlightColor = std::make_shared<QColor>(bg);
    }

    this->appendChannelName();
    this->emplace<TimestampElement>(time.time());
    appendBadges(*this, item.authorBadges);

    QColor nameColor = getSettings()->youtubeSuperChatWhiteName
                           ? QColor(255, 255, 255)
                       : item.authorNameTextColor != 0
                           ? QColor::fromRgba(item.authorNameTextColor)
                           : userColor;
    auto *nameElement = this->emplace<TextElement>(
        item.authorName, MessageElementFlags{MessageElementFlag::Username},
        nameColor, FontStyle::ChatMediumBold);
    if (!item.authorName.isEmpty())
    {
        nameElement->setLink({Link::UserInfo, item.authorName});
    }

    if (!item.amountText.isEmpty())
    {
        this->emplace<TextElement>(
            item.amountText, MessageElementFlags{MessageElementFlag::Text},
            MessageColor::Text, FontStyle::ChatMediumBold);
    }

    if (!item.stickerUrl.isEmpty())
    {
        this->appendEmote(makeStickerEmote(item.stickerUrl));
    }

    QString messageText;
    appendMessageRuns(*this, item.runs, messageText);

    QString summary = item.authorName % u" · " % item.amountText;
    this->message().messageText =
        messageText.isEmpty() ? summary : (summary % u": " % messageText);
    this->message().searchText = this->message().messageText;
}

void YouTubeMessageBuilder::buildMembership(const YouTubeChatItem &item,
                                            const QDateTime &time,
                                            const QColor &userColor)
{
    if (getSettings()->highlightYouTubeMemberships)
    {
        QColor green(15, 157, 88);  // YouTube membership green
        green.setAlpha(MEMBERSHIP_HIGHLIGHT_ALPHA);
        this->message().flags.set(MessageFlag::Subscription);
        this->message().flags.set(MessageFlag::Highlighted);
        this->message().highlightColor = std::make_shared<QColor>(green);
    }

    this->appendChannelName();
    this->emplace<TimestampElement>(time.time());
    appendBadges(*this, item.authorBadges);

    if (!item.authorName.isEmpty())
    {
        auto *nameElement = this->emplace<TextElement>(
            item.authorName, MessageElementFlags{MessageElementFlag::Username},
            userColor, FontStyle::ChatMediumBold);
        nameElement->setLink({Link::UserInfo, item.authorName});
    }

    QString messageText;
    appendMessageRuns(*this, item.runs, messageText);
    this->message().messageText = item.authorName.isEmpty()
                                      ? messageText
                                      : (item.authorName % u' ' % messageText);
    this->message().searchText = this->message().messageText;
}

void YouTubeMessageBuilder::appendChannelName()
{
    QString channelName(u'#' % this->channel()->getName());
    Link link(Link::JumpToChannel, this->channel()->getName());

    this->emplace<TextElement>(channelName, MessageElementFlag::ChannelName,
                               MessageColor::System)
        ->setLink(link);
}

void YouTubeMessageBuilder::appendUsername(const QString &username,
                                           const MessageColor &color)
{
    auto *element = this->emplace<TextElement>(
        username % u':', MessageElementFlags{MessageElementFlag::Username},
        color, FontStyle::ChatMediumBold);
    element->setLink({Link::UserInfo, username});
}

}  // namespace chatterino

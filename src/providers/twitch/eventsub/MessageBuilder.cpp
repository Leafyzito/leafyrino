// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/eventsub/MessageBuilder.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "util/Helpers.hpp"

#include <QStringBuilder>

#include <algorithm>
#include <limits>

namespace {

using namespace chatterino;
using namespace chatterino::eventsub;
using namespace chatterino::literals;

/// <MODERATOR> turned {on/off} <MODE> mode. [<DURATION>]
void makeModeMessage(EventSubMessageBuilder &builder,
                     const lib::payload::channel_moderate::v2::Event &event,
                     const QString &mode, bool on, const QString &duration = {})
{
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate(u"turned"_s, text);
    QString op = on ? u"on"_s : u"off"_s;
    builder.emplaceSystemTextAndUpdate(op, text);
    builder.emplaceSystemTextAndUpdate(mode, text);
    builder.emplaceSystemTextAndUpdate(u"mode."_s, text);

    if (!duration.isEmpty())
    {
        builder.emplaceSystemTextAndUpdate(duration, text);
    }

    builder.setMessageAndSearchText(text);
}

// XXX: this is a duplicate from messages/MessageBuilder.cpp
EmotePtr makeAutoModBadge()
{
    return std::make_shared<Emote>(Emote{
        .name = EmoteName{},
        .images =
            ImageSet{Image::fromResourcePixmap(getResources().twitch.automod)},
        .tooltip = Tooltip{"AutoMod"},
        .homePage =
            Url{"https://dashboard.twitch.tv/settings/moderation/automod"},
    });
}

QString localizedDisplayName(const automod::ReviewItem &item)
{
    QString displayName =
        item.userName.isEmpty() ? item.userLogin : item.userName;
    const bool hasLocalizedName =
        displayName.compare(item.userLogin, Qt::CaseInsensitive) != 0;

    switch (getSettings()->usernameDisplayMode.getValue())
    {
        case UsernameDisplayMode::Username:
            return item.userLogin;
        case UsernameDisplayMode::UsernameAndLocalizedName:
            if (hasLocalizedName)
            {
                return item.userLogin % u'(' % displayName % u')';
            }
            break;
        case UsernameDisplayMode::LocalizedName:
        default:
            break;
    }
    return displayName;
}

/// Adds the held message word by word, tagging each element with where its
/// text starts in Message::messageText (the held text starts at
/// `baseOffset` there) so the flagged ranges can be marked
void appendHeldMessageText(EventSubMessageBuilder &builder,
                           TwitchChannel *channel, const QString &message,
                           qsizetype baseOffset)
{
    qsizetype wordStart = 0;
    while (wordStart <= message.size())
    {
        auto wordEnd = message.indexOf(u' ', wordStart);
        if (wordEnd < 0)
        {
            wordEnd = message.size();
        }
        const auto word =
            QStringView(message).sliced(wordStart, wordEnd - wordStart);
        if (!word.isEmpty())
        {
            const auto firstNew = builder->elements.size();
            builder.addWordFromUserMessage(word, channel);

            // a word can turn into several elements (e.g. "@user," is a
            // mention followed by text)
            qsizetype consumed = 0;
            for (auto i = firstNew; i < builder->elements.size(); ++i)
            {
                auto *text =
                    dynamic_cast<TextElement *>(builder->elements[i].get());
                if (text == nullptr)
                {
                    continue;
                }
                const auto words = text->words();
                const auto first = words.isEmpty() ? QString{} : words.front();
                auto pos = first.isEmpty() ? -1
                                           : word.indexOf(first, consumed,
                                                          Qt::CaseInsensitive);
                if (pos < 0)
                {
                    pos = consumed;
                }
                text->messageTextOffset = baseOffset + wordStart + pos;
                consumed = pos + first.size();
            }
        }
        wordStart = wordEnd + 1;
    }
}

qsizetype utf16OffsetForCodepoint(QStringView text, int codepointOffset)
{
    if (codepointOffset < 0)
    {
        return -1;
    }
    qsizetype utf16 = 0;
    int codepoints = 0;
    while (utf16 < text.size() && codepoints < codepointOffset)
    {
        const auto first = text.at(utf16);
        if (first.isHighSurrogate() && utf16 + 1 < text.size() &&
            text.at(utf16 + 1).isLowSurrogate())
        {
            utf16 += 2;
        }
        else
        {
            ++utf16;
        }
        ++codepoints;
    }
    return codepoints == codepointOffset ? utf16 : -1;
}

/// Twitch reports flagged ranges as inclusive codepoint offsets, store them as
/// UTF-16 ranges of the message text
void appendTwitchMatch(automod::HoldData &data,
                       const lib::automod::Boundary &boundary,
                       const QString &ruleDetails)
{
    if (boundary.startPos < 0 || boundary.endPos < boundary.startPos ||
        boundary.endPos == std::numeric_limits<int>::max())
    {
        return;
    }
    const auto start =
        utf16OffsetForCodepoint(data.messageText, boundary.startPos);
    const auto end =
        utf16OffsetForCodepoint(data.messageText, boundary.endPos + 1);
    if (start < 0 || end <= start)
    {
        return;
    }
    const auto fragment = data.messageText.mid(start, end - start);
    if (!fragment.isEmpty() &&
        !data.matchedFragments.contains(fragment, Qt::CaseInsensitive))
    {
        data.matchedFragments.push_back(fragment);
    }
    data.twitchMatches.push_back({
        .start = start,
        .length = end - start,
        .color = defaultAutoModMatchColor(),
        .ruleName = ruleDetails,
        .pattern = fragment,
        .source = HighlightMatchSource::AutoMod,
        .style = HighlightMatchStyle::Fill,
    });
}

/// Joins overlapping matches and matches only separated by whitespace, so a
/// flagged phrase is marked as one piece
void mergeAdjacentTwitchMatches(automod::HoldData &data)
{
    if (data.twitchMatches.size() < 2)
    {
        return;
    }

    std::ranges::sort(data.twitchMatches, {}, &HighlightMatch::start);
    std::vector<HighlightMatch> merged;
    merged.reserve(data.twitchMatches.size());
    for (auto &match : data.twitchMatches)
    {
        if (!merged.empty())
        {
            auto &previous = merged.back();
            const auto previousEnd = previous.start + previous.length;
            const auto matchEnd = match.start + match.length;
            const auto gapLength = match.start - previousEnd;
            const bool whitespaceGap =
                gapLength >= 0 && data.messageText.mid(previousEnd, gapLength)
                                      .trimmed()
                                      .isEmpty();
            if (previous.ruleName == match.ruleName &&
                (match.start <= previousEnd || whitespaceGap))
            {
                previous.length =
                    std::max(previousEnd, matchEnd) - previous.start;
                previous.pattern =
                    data.messageText.mid(previous.start, previous.length);
                continue;
            }
        }
        merged.emplace_back(std::move(match));
    }
    data.twitchMatches = std::move(merged);
}

/// "sexual_content" -> "Sexual content"
QString displayAutoModCategory(QString category)
{
    category.replace(u'_', u' ');
    if (!category.isEmpty())
    {
        category[0] = category.at(0).toUpper();
    }
    return category;
}

void fillAutoModReason(automod::HoldData &data,
                       const lib::automod::AutomodReason &reason)
{
    data.blockedTerm = false;
    data.reasonCategory = displayAutoModCategory(reason.category.qt());
    data.reasonLevel = reason.level;
    data.reasonSummary =
        data.reasonCategory % u" · Level " % QString::number(reason.level);
    const QString details = u"Twitch AutoMod<br>"_s % data.reasonSummary;
    for (const auto &boundary : reason.boundaries)
    {
        appendTwitchMatch(data, boundary, details);
    }
    mergeAdjacentTwitchMatches(data);
}

void fillAutoModReason(automod::HoldData &data,
                       const lib::automod::BlockedTermReason &reason)
{
    data.blockedTerm = true;
    data.reasonCategory = u"Blocked term"_s;
    for (const auto &found : reason.termsFound)
    {
        const auto owner =
            QString::fromStdString(found.ownerBroadcasterUserLogin);
        auto details = u"Twitch AutoMod<br>Blocked term"_s;
        if (!owner.isEmpty() &&
            owner.compare(data.broadcasterLogin, Qt::CaseInsensitive) != 0)
        {
            details += u"<br>Shared from #"_s % owner.toHtmlEscaped();
        }
        appendTwitchMatch(data, found.boundary, details);
        if (data.termOwnerLogin.isEmpty() && !owner.isEmpty())
        {
            data.termOwnerLogin = owner;
        }
    }
    mergeAdjacentTwitchMatches(data);
    const auto count = data.matchedFragments.size();
    if (count == 0)
    {
        data.reasonSummary = u"Blocked term usage"_s;
    }
    else if (count == 1)
    {
        data.reasonSummary = u"Matched 1 blocked term"_s;
    }
    else
    {
        data.reasonSummary = u"Matched %1 blocked terms"_s.arg(count);
    }
}

/// Reasons Twitch only sends as a string, e.g. "blocked_link"
void fillAutoModReason(automod::HoldData &data, const std::string &reason)
{
    data.blockedTerm = false;
    data.reasonCategory =
        displayAutoModCategory(QString::fromStdString(reason));
    data.reasonSummary = data.reasonCategory;
}

/// "Matched 1 blocked term "leafytest"" or "Swearing · Level 4"
QString autoModReasonText(const automod::ReviewItem &item)
{
    auto reason = item.reasonSummary;
    if (item.matchedFragments.isEmpty())
    {
        return reason;
    }
    // the blocked term itself can be hidden while streaming
    if (item.blockedTerm && getSettings()->streamerModeHideBlockedTermText &&
        getApp()->getStreamerMode()->isEnabled())
    {
        return reason;
    }
    return reason % u" \"" % item.matchedFragments.join(u"\", \""_s) % u'"';
}

/// The action row of a review, or its state when it can't be acted on
void appendAutoModReviewActions(EventSubMessageBuilder &builder,
                                const automod::ReviewItem &item,
                                bool withTimeouts, FontStyle statusStyle)
{
    const MessageElementFlags flags{MessageElementFlag::Text};
    const bool showActionRow =
        item.actionsAllowed && item.state == automod::ReviewState::Open;
    const bool pending = item.state == automod::ReviewState::Approving ||
                         item.state == automod::ReviewState::Denying ||
                         item.secondaryInProgress;

    if (showActionRow)
    {
        builder
            .emplace<TextElement>(withTimeouts ? u"Approve"_s : u"Allow"_s,
                                  flags, MessageColor(QColor(0, 255, 0)),
                                  FontStyle::ChatMediumBold)
            ->setLink({Link::AutoModReviewApprove, item.key})
            ->setTooltip(u"Allow this message"_s);
        builder
            .emplace<TextElement>(u"Deny"_s, flags,
                                  MessageColor(QColor(255, 0, 0)),
                                  FontStyle::ChatMediumBold)
            ->setLink({Link::AutoModReviewDeny, item.key})
            ->setTooltip(u"Deny this message"_s);
        if (!withTimeouts)
        {
            return;
        }

        builder.emplace<TextElement>(u"·"_s, flags, MessageColor::System,
                                     FontStyle::ChatSmall);
        const auto addTimeout = [&](int seconds, const QString &tooltip) {
            builder
                .emplace<AutoModActionElement>(AutoModActionKind::Timeout,
                                               seconds)
                ->setLink({Link::AutoModReviewTimeout,
                           item.key % u'\n' % QString::number(seconds)})
                ->setTooltip(tooltip);
        };
        addTimeout(30, u"Deny and timeout for 30 seconds"_s);
        addTimeout(60, u"Deny and timeout for 1 minute"_s);
        addTimeout(5 * 60, u"Deny and timeout for 5 minutes"_s);
        addTimeout(10 * 60, u"Deny and timeout for 10 minutes"_s);
        addTimeout(60 * 60, u"Deny and timeout for 1 hour"_s);
        addTimeout(24 * 60 * 60, u"Deny and timeout for 1 day"_s);
        builder
            .emplace<TextElement>(u"Other…"_s, flags, MessageColor::Link,
                                  FontStyle::ChatSmall)
            ->setLink({Link::AutoModReviewTimeout, item.key})
            ->setTooltip(u"Deny and timeout for a custom duration"_s);
        builder.emplace<AutoModActionElement>(AutoModActionKind::Ban)
            ->setLink({Link::AutoModReviewBan, item.key})
            ->setTooltip(u"Deny and permanently ban this user"_s);
        return;
    }

    if (pending)
    {
        builder.emplace<TextElement>(automod::reviewStateLabel(item), flags,
                                     MessageColor::System,
                                     FontStyle::ChatMediumBold);
        return;
    }

    if (item.state == automod::ReviewState::Open && !item.actionsAllowed)
    {
        builder.emplace<TextElement>(u"Read only"_s, flags,
                                     MessageColor::System, statusStyle);
        return;
    }

    if (item.actionsAllowed &&
        (item.failedAction != automod::ReviewAction::None ||
         item.secondaryFailed))
    {
        const auto failedAction =
            item.secondaryFailed ? item.secondaryAction : item.failedAction;
        auto failureText = automod::reviewActionLabel(failedAction);
        if (!failureText.isEmpty())
        {
            failureText[0] = failureText[0].toUpper();
        }
        failureText += u" failed"_s;
        if (!item.statusDetail.isEmpty())
        {
            failureText += u" · "_s % item.statusDetail;
        }
        builder
            .emplace<TextElement>(failureText, flags, MessageColor::System,
                                  statusStyle)
            ->setTooltip(item.statusDetail);
        builder
            .emplace<TextElement>(u"Retry"_s, flags, MessageColor::Link,
                                  FontStyle::ChatMediumBold)
            ->setLink({Link::AutoModReviewRetry, item.key})
            ->setTooltip(u"Try the failed action again"_s);
        return;
    }

    if (item.state == automod::ReviewState::Approved ||
        item.state == automod::ReviewState::Denied)
    {
        QString resolution = item.state == automod::ReviewState::Approved
                                 ? u"Approved"_s
                                 : u"Denied"_s;
        const auto moderator = item.moderatorName.isEmpty()
                                   ? item.moderatorLogin
                                   : item.moderatorName;
        if (!moderator.isEmpty())
        {
            resolution += u" by " % moderator;
        }
        if (!item.statusDetail.isEmpty() &&
            item.secondaryAction != automod::ReviewAction::None)
        {
            resolution += u" · " % item.statusDetail;
        }
        builder.emplace<TextElement>(resolution, flags, MessageColor::System,
                                     statusStyle);
        return;
    }

    const auto status =
        item.statusDetail.isEmpty()
            ? automod::reviewStateLabel(item)
            : automod::reviewStateLabel(item) % u" · " % item.statusDetail;
    builder.emplace<TextElement>(status, flags, MessageColor::System,
                                 statusStyle);
}

void appendAutoModReviewContext(EventSubMessageBuilder &builder,
                                const automod::ReviewItem &item,
                                bool isReviewQueue)
{
    if (!getSettings()->autoModReviewShowContext || item.context.isEmpty())
    {
        return;
    }

    const MessageElementFlags expandedFlags{
        MessageElementFlag::Text, MessageElementFlag::AutoModReviewExpanded};
    const auto style =
        isReviewQueue ? FontStyle::ChatSmall : FontStyle::ChatMedium;

    builder.emplace<LinebreakElement>(expandedFlags);
    builder.emplace<TextElement>(u"Earlier messages"_s, expandedFlags,
                                 MessageColor::System, FontStyle::ChatSmall);
    const auto addContext = [&](const automod::ContextMessage &context) {
        builder.emplace<LinebreakElement>(expandedFlags);
        builder.emplace<TextElement>(
            context.receivedAt.isValid()
                ? context.receivedAt.toLocalTime().time().toString(u"HH:mm"_s)
                : u"Earlier"_s,
            expandedFlags, MessageColor::System, style);
        builder.emplace<TextElement>(context.displayName + ':', expandedFlags,
                                     MessageColor::Link, style);
        builder.emplace<TextElement>(context.text, expandedFlags,
                                     MessageColor::Text, style);
    };
    // 0 = newest at top
    if (getSettings()->autoModReviewContextOrder != 1)
    {
        for (auto it = item.context.crbegin(); it != item.context.crend(); ++it)
        {
            addContext(*it);
        }
    }
    else
    {
        for (const auto &context : item.context)
        {
            addContext(context);
        }
    }
}

void appendAutoModShortcutHints(EventSubMessageBuilder &builder,
                                const automod::ReviewItem &item)
{
    const MessageElementFlags expandedFlags{
        MessageElementFlag::Text, MessageElementFlag::AutoModReviewExpanded};
    builder.emplace<LinebreakElement>(expandedFlags);
    const auto addHint = [&](const QString &key, const QString &action,
                             bool separator) {
        builder.emplace<TextElement>(key, expandedFlags, MessageColor::Link,
                                     FontStyle::ChatSmall);
        builder.emplace<TextElement>(
            action, expandedFlags, MessageColor::System, FontStyle::ChatSmall);
        if (separator)
        {
            builder.emplace<TextElement>(u"·"_s, expandedFlags,
                                         MessageColor::System,
                                         FontStyle::ChatSmall);
        }
    };
    addHint(u"↑/↓"_s, u"Select"_s, true);
    if (item.actionsAllowed && item.state == automod::ReviewState::Open)
    {
        addHint(u"A"_s, u"Approve"_s, true);
        addHint(u"D"_s, u"Deny"_s, true);
        addHint(u"T"_s, u"Timeout"_s, true);
        addHint(u"B"_s, u"Ban"_s, true);
    }
    addHint(u"Enter"_s, u"Usercard"_s, false);
}

}  // namespace

namespace chatterino::eventsub {

EventSubMessageBuilder::EventSubMessageBuilder(TwitchChannel *channel,
                                               const QDateTime &time)
    : channel(channel)
{
    this->emplace<TimestampElement>(time.time());
    this->message().flags.set(MessageFlag::System, MessageFlag::EventSub);
    this->message().timestamp = time;
}

EventSubMessageBuilder::EventSubMessageBuilder(TwitchChannel *channel)
    : channel(channel)
{
    this->message().flags.set(MessageFlag::EventSub);
}

EventSubMessageBuilder::~EventSubMessageBuilder() = default;

void EventSubMessageBuilder::appendUser(const lib::String &userName,
                                        const lib::String &userLogin,
                                        QString &text, bool trailingSpace)
{
    auto login = userLogin.qt();
    auto *el = this->emplace<MentionElement>(
        userName.qt(), login, MessageColor::System,
        this->channel->getUserColor(login));
    text.append(login);

    if (trailingSpace)
    {
        text.append(u' ');
    }
    else
    {
        el->setTrailingSpace(false);
    }
}

void EventSubMessageBuilder::setMessageAndSearchText(const QString &text)
{
    assert(this->message().messageText.isNull());
    assert(this->message().searchText.isNull());

    this->message().messageText = text;
    this->message().searchText = text;
}

void makeModerateMessage(EventSubMessageBuilder &builder,
                         const lib::payload::channel_moderate::v2::Event &event,
                         const lib::payload::channel_moderate::v2::Vip &action)
{
    builder->timeoutUser = action.userLogin.qt();
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate("has added", text);
    builder.appendUser(action.userName, action.userLogin, text);
    builder.emplaceSystemTextAndUpdate("as a VIP of this channel.", text);

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Unvip &action)
{
    builder->timeoutUser = action.userLogin.qt();
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate("has removed", text);
    builder.appendUser(action.userName, action.userLogin, text);
    builder.emplaceSystemTextAndUpdate("as a VIP of this channel.", text);

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(EventSubMessageBuilder &builder,
                         const lib::payload::channel_moderate::v2::Event &event,
                         const lib::payload::channel_moderate::v2::Warn &action)
{
    builder->timeoutUser = action.userLogin.qt();
    builder->flags.set(MessageFlag::ModerationAction);
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate("has warned", text);
    builder.appendUser(action.userName, action.userLogin, text, false);

    QStringList reasons;

    if (!action.reason.qt().isEmpty())
    {
        reasons.append(action.reason.qt());
    }

    for (const auto &rule : action.chatRulesCited)
    {
        if (!rule.qt().isEmpty())
        {
            reasons.append(rule.qt());
        }
    }

    if (reasons.isEmpty())
    {
        builder.emplaceSystemTextAndUpdate(".", text);
    }
    else
    {
        builder.emplaceSystemTextAndUpdate(":", text);
        builder.emplaceSystemTextAndUpdate(reasons.join(", "), text);
    }

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Unban &action)
{
    builder->flags.set(MessageFlag::ModerationAction, MessageFlag::Untimeout);

    QString text;
    bool isShared = event.isFromSharedChat();

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate("unbanned", text);
    builder.appendUser(action.userName, action.userLogin, text, isShared);

    if (isShared)
    {
        builder.emplaceSystemTextAndUpdate("in", text);
        builder.appendUser(*event.sourceBroadcasterUserName,
                           *event.sourceBroadcasterUserLogin, text, false);
    }

    builder.emplaceSystemTextAndUpdate(".", text);

    builder.setMessageAndSearchText(text);
    builder->timeoutUser = action.userLogin.qt();
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Untimeout &action)
{
    builder->flags.set(MessageFlag::ModerationAction, MessageFlag::Untimeout);

    QString text;
    bool isShared = event.isFromSharedChat();

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate("untimedout", text);
    builder.appendUser(action.userName, action.userLogin, text, isShared);

    if (isShared)
    {
        builder.emplaceSystemTextAndUpdate("in", text);
        builder.appendUser(*event.sourceBroadcasterUserName,
                           *event.sourceBroadcasterUserLogin, text, false);
    }

    builder.emplaceSystemTextAndUpdate(".", text);

    builder.setMessageAndSearchText(text);
    builder->timeoutUser = action.userLogin.qt();
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Delete &action)
{
    builder->flags.set(MessageFlag::DoNotTriggerNotification,
                       MessageFlag::ModerationAction);

    (void)event;
    builder->loginName.clear();
    QString text;
    builder.emplace<TextElement>("A message from", MessageElementFlag::Text,
                                 MessageColor::System);
    builder
        .emplace<TextElement>(action.userName.qt(),
                              MessageElementFlag::Username,
                              MessageColor::System, FontStyle::ChatMediumBold)
        ->setLink({Link::UserInfo, action.userLogin.qt()});
    builder.emplace<TextElement>("was deleted:", MessageElementFlag::Text,
                                 MessageColor::System);

    auto limit = getSettings()->deletedMessageLengthLimit.getValue();
    if (limit > 0 && action.messageBody.view().length() > limit)
    {
        builder
            .emplace<TextElement>(action.messageBody.qt().left(limit) + "…",
                                  MessageElementFlag::Text, MessageColor::Text)
            ->setLink({Link::JumpToMessage, action.messageID.qt()});

        text.append(action.messageBody.qt().left(limit) + "…");
    }
    else
    {
        builder
            .emplace<TextElement>(action.messageBody.qt(),
                                  MessageElementFlag::Text, MessageColor::Text)
            ->setLink({Link::JumpToMessage, action.messageID.qt()});

        text.append(action.messageBody.qt());
    }

    text = QString("A message from %1 was deleted: %2")
               .arg(action.userLogin.qt(), text);
    builder.setMessageAndSearchText(text);
    builder->timeoutUser = action.userLogin.qt();
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Followers &action)
{
    QString duration;
    if (action.followDurationMinutes > 0)
    {
        duration = u"(%1 minutes)"_s.arg(action.followDurationMinutes);
    }
    makeModeMessage(builder, event, u"followers-only"_s, true, duration);
}
void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::FollowersOff & /*action*/)
{
    makeModeMessage(builder, event, u"followers-only"_s, false);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::EmoteOnly & /*action*/)
{
    makeModeMessage(builder, event, u"emote-only"_s, true);
}
void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::EmoteOnlyOff & /*action*/)
{
    makeModeMessage(builder, event, u"emote-only"_s, false);
}

void makeModerateMessage(EventSubMessageBuilder &builder,
                         const lib::payload::channel_moderate::v2::Event &event,
                         const lib::payload::channel_moderate::v2::Slow &action)
{
    makeModeMessage(builder, event, u"slow"_s, true,
                    u"(%1 seconds)"_s.arg(action.waitTimeSeconds));
}
void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::SlowOff & /*action*/)
{
    makeModeMessage(builder, event, u"slow"_s, false);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Subscribers & /*action*/)
{
    makeModeMessage(builder, event, u"subscribers-only"_s, true);
}
void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::SubscribersOff & /*action*/)
{
    makeModeMessage(builder, event, u"subscribers-only"_s, false);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Uniquechat & /*action*/)
{
    makeModeMessage(builder, event, u"unique-chat"_s, true);
}
void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::UniquechatOff & /*action*/)
{
    makeModeMessage(builder, event, u"unique-chat"_s, false);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::AutomodTerms &action)
{
    builder->flags.set(MessageFlag::ModerationAction);

    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    if (action.action == "add")
    {
        builder.emplaceSystemTextAndUpdate(u"added"_s, text);
    }
    else
    {
        builder.emplaceSystemTextAndUpdate(u"removed"_s, text);
    }

    QString terms;
    for (size_t i = 0; i < action.terms.size(); i++)
    {
        if (i != 0)
        {
            if (i == action.terms.size() - 1)
            {
                if (action.terms.size() == 2)
                {
                    terms.append(u" and ");
                }
                else
                {
                    terms.append(u", and ");
                }
            }
            else
            {
                terms.append(u", ");
            }
        }
        terms.append(u'"');
        terms.append(action.terms[i].qt());
        terms.append(u'"');
    }
    builder.emplaceSystemTextAndUpdate(terms, text);
    builder.emplaceSystemTextAndUpdate(u"as"_s, text);
    if (action.terms.size() == 1)
    {
        builder.emplaceSystemTextAndUpdate(u"a"_s, text);
    }
    builder.emplaceSystemTextAndUpdate(action.list.qt(), text);
    if (action.terms.size() == 1)
    {
        builder.emplaceSystemTextAndUpdate(u"term"_s, text);
    }
    else
    {
        builder.emplaceSystemTextAndUpdate(u"terms"_s, text);
    }
    builder.emplaceSystemTextAndUpdate(u"on AutoMod."_s, text);

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(EventSubMessageBuilder &builder,
                         const lib::payload::channel_moderate::v2::Event &event,
                         const lib::payload::channel_moderate::v2::Mod &action)
{
    builder->timeoutUser = action.userLogin.qt();
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate(u"modded"_s, text);
    builder.appendUser(action.userName, action.userLogin, text, false);
    builder.emplaceSystemTextAndUpdate(u"."_s, text);

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Unmod &action)
{
    builder->timeoutUser = action.userLogin.qt();
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate(u"unmodded"_s, text);
    builder.appendUser(action.userName, action.userLogin, text, false);
    builder.emplaceSystemTextAndUpdate(u"."_s, text);

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(EventSubMessageBuilder &builder,
                         const lib::payload::channel_moderate::v2::Event &event,
                         const lib::payload::channel_moderate::v2::Raid &action)
{
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate("initiated a raid to", text);
    builder.appendUser(action.userName, action.userLogin, text, false);

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Unraid &action)
{
    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate("canceled the raid to", text);
    builder.appendUser(action.userName, action.userLogin, text, false);

    builder.setMessageAndSearchText(text);
}

void makeModerateMessage(
    EventSubMessageBuilder &builder,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::UnbanRequest &action)
{
    builder->timeoutUser = action.userLogin.qt();
    builder->flags.set(MessageFlag::ModerationAction);

    QString text;

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    if (action.isApproved)
    {
        builder.emplaceSystemTextAndUpdate("approved", text);
    }
    else
    {
        builder.emplaceSystemTextAndUpdate("denied", text);
    }
    builder.appendOrEmplaceSystemTextAndUpdate("the unban request from", text);
    builder.appendUser(action.userName, action.userLogin, text, false);
    if (action.moderatorMessage.isEmpty())
    {
        builder.emplaceSystemTextAndUpdate(".", text);
    }
    else
    {
        builder.emplaceSystemTextAndUpdate(":", text);
        builder.appendOrEmplaceSystemTextAndUpdate(action.moderatorMessage.qt(),
                                                   text);
    }

    builder.setMessageAndSearchText(text);
}

automod::HoldData makeAutoModReviewHoldData(
    QString notificationID, const QDateTime &time,
    const lib::payload::automod_message_hold::v2::Event &event)
{
    automod::HoldData data;
    data.notificationID = std::move(notificationID);
    data.broadcasterID = event.broadcasterUserID.qt();
    data.broadcasterLogin = event.broadcasterUserLogin.qt();
    data.broadcasterName = event.broadcasterUserName.qt();
    data.userID = event.userID.qt();
    data.userLogin = event.userLogin.qt();
    data.userName = event.userName.qt();
    data.messageID = event.messageID.qt();
    data.messageText = event.message.text.qt();
    data.receivedAt = time;
    std::visit(
        [&](const auto &reason) {
            fillAutoModReason(data, reason);
        },
        event.reason);
    return data;
}

automod::UpdateData makeAutoModReviewUpdateData(
    QString notificationID, const QDateTime &time,
    const lib::payload::automod_message_update::v2::Event &event)
{
    automod::UpdateData data;
    data.notificationID = std::move(notificationID);
    data.broadcasterID = event.broadcasterUserID.qt();
    data.broadcasterLogin = event.broadcasterUserLogin.qt();
    data.broadcasterName = event.broadcasterUserName.qt();
    data.userID = QString::fromStdString(event.userID);
    data.userLogin = QString::fromStdString(event.userLogin);
    data.userName = QString::fromStdString(event.userName);
    data.messageID = event.messageID.qt();
    data.messageText = event.message.text.qt();
    data.receivedAt = time;
    data.status = QString::fromStdString(event.status);
    data.moderatorID = QString::fromStdString(event.moderatorUserID);
    data.moderatorLogin = QString::fromStdString(event.moderatorUserLogin);
    data.moderatorName = QString::fromStdString(event.moderatorUserName);
    std::visit(
        [&](const auto &reason) {
            fillAutoModReason(data, reason);
        },
        event.reason);
    return data;
}

std::pair<MessagePtr, HighlightAlert> makeAutoModReviewMessage(
    TwitchChannel *channel, const automod::ReviewItem &item,
    AutoModReviewPresentation presentation)
{
    EventSubMessageBuilder builder(channel);
    builder->timestamp = item.receivedAt;
    builder->id = u"automod_" % item.messageID;
    builder->loginName = item.userLogin;
    builder->displayName = item.userName;
    builder->userID = item.userID;
    builder->channelName = item.broadcasterLogin;
    builder->flags.set(MessageFlag::PubSub, MessageFlag::ModerationAction,
                       MessageFlag::AutoMod,
                       MessageFlag::AutoModOffendingMessageHeader,
                       MessageFlag::AutoModOffendingMessage);
    builder->flags.set(MessageFlag::AutoModBlockedTerm, item.blockedTerm);
    builder->autoModReview = std::make_shared<const automod::ReviewItem>(item);

    const auto reasonText = autoModReasonText(item);

    // logs write "user: <messageText>", keep it clear the user didn't say
    // this in chat
    const QString messageTextPrefix =
        u"AutoMod: Held a message for "_s % reasonText % u": ";
    if (item.highlightMatches)
    {
        auto shifted = *item.highlightMatches;
        for (auto &match : shifted)
        {
            match.start += messageTextPrefix.size();
        }
        builder->highlightMatches =
            std::make_shared<const std::vector<HighlightMatch>>(
                std::move(shifted));
    }

    const MessageElementFlags textFlags{MessageElementFlag::Text};
    const auto userColor =
        channel != nullptr ? channel->getUserColor(item.userLogin) : QColor{};
    const auto appendChannelName = [&](FontStyle style) {
        builder
            .emplace<TextElement>(u'#' + item.broadcasterLogin,
                                  MessageElementFlag::ChannelName,
                                  MessageColor::System, style)
            ->setLink({Link::JumpToChannel, item.broadcasterLogin});
    };
    const auto appendSender = [&] {
        builder
            .emplace<MentionElement>(localizedDisplayName(item) + ':',
                                     item.userLogin, MessageColor::Text,
                                     userColor)
            ->addFlags(MessageElementFlag::Username);
        appendHeldMessageText(builder, channel, item.messageText,
                              messageTextPrefix.size());
    };

    switch (presentation)
    {
        case AutoModReviewPresentation::ChatLegacy: {
            builder.emplace<BadgeElement>(
                makeAutoModBadge(), MessageElementFlag::BadgeChannelAuthority);
            builder.emplace<TextElement>(u"AutoMod:"_s, textFlags,
                                         QColor(0, 0, 255),
                                         FontStyle::ChatMediumBold);
            builder.emplace<TextElement>(u"Held a message for reason: "_s %
                                             reasonText %
                                             u". Allow will post it in chat.",
                                         textFlags, MessageColor::Text);
            appendAutoModReviewActions(builder, item, false,
                                       FontStyle::ChatMedium);
            builder.emplace<LinebreakElement>(textFlags);

            appendChannelName(FontStyle::ChatMedium);
            builder.emplace<TimestampElement>(item.receivedAt.time());
            builder.emplace<TwitchModerationElement>();
            appendSender();
        }
        break;

        case AutoModReviewPresentation::ChatAdvanced: {
            builder.emplace<BadgeElement>(
                makeAutoModBadge(), MessageElementFlag::BadgeChannelAuthority);
            builder.emplace<TextElement>(u"AutoMod:"_s, textFlags,
                                         QColor(0, 0, 255),
                                         FontStyle::ChatMediumBold);
            builder.emplace<TextElement>(
                getSettings()->autoModReviewShowReason
                    ? u"Held a message for "_s % reasonText % u'.'
                    : u"Held a message."_s,
                textFlags, MessageColor::System);
            builder.emplace<LinebreakElement>(textFlags);

            appendChannelName(FontStyle::ChatMedium);
            builder.emplace<TimestampElement>(item.receivedAt.time());
            builder.emplace<TwitchModerationElement>();
            appendSender();
            appendAutoModReviewContext(builder, item, false);

            builder.emplace<LinebreakElement>(textFlags);
            appendAutoModReviewActions(builder, item, true,
                                       FontStyle::ChatSmall);
        }
        break;

        case AutoModReviewPresentation::ReviewQueue: {
            builder
                .emplace<TextElement>(u'#' + item.broadcasterLogin, textFlags,
                                      MessageColor::Link, FontStyle::ChatSmall)
                ->setLink({Link::JumpToChannel, item.broadcasterLogin});
            if (getSettings()->autoModReviewShowReason && !reasonText.isEmpty())
            {
                builder.emplace<TextElement>(u"·"_s, textFlags,
                                             MessageColor::System,
                                             FontStyle::ChatSmall);
                builder.emplace<TextElement>(reasonText, textFlags,
                                             MessageColor::System,
                                             FontStyle::ChatSmall);
            }
            builder.emplace<LinebreakElement>(textFlags);

            builder.emplace<TimestampElement>(item.receivedAt.time());
            builder.emplace<TwitchModerationElement>();
            appendSender();
            appendAutoModReviewContext(builder, item, true);

            builder.emplace<LinebreakElement>(textFlags);
            appendAutoModReviewActions(builder, item, true,
                                       FontStyle::ChatSmall);
            if (getSettings()->autoModReviewShowShortcutHints)
            {
                appendAutoModShortcutHints(builder, item);
            }
        }
        break;
    }

    builder->messageText = messageTextPrefix % item.messageText;
    builder->searchText = item.broadcasterLogin % u' ' % item.userLogin % u' ' %
                          item.messageText % u' ' % reasonText % u' ' %
                          automod::reviewStateLabel(item);

    // a review without a channel (e.g. left since) only lives in /automod
    HighlightAlert alert;
    if (channel != nullptr)
    {
        QVariantMap tagsMap;
        Communi::TagsRef tags(tagsMap);
        alert = builder.parseHighlights(tags, item.messageText, {}, channel);
    }

    return {builder.release(), alert};
}

std::pair<MessagePtr, HighlightAlert> makeSuspiciousUserMessage(
    TwitchChannel *channel, const QDateTime &time,
    const lib::payload::channel_suspicious_user_message::v1::Event &event)
{
    EventSubMessageBuilder builder(channel);

    // Builder for low trust user message with explanation
    builder->channelName = event.broadcasterUserLogin.qt();
    builder->timestamp = time;
    builder->loginName = event.userLogin.qt();
    builder->flags.set(MessageFlag::PubSub, MessageFlag::LowTrustUsers);

    // AutoMod shield badge
    builder.emplace<BadgeElement>(makeAutoModBadge(),
                                  MessageElementFlag::BadgeChannelAuthority);

    // Suspicious user header message
    QString prefix = u"Suspicious User:"_s;
    builder.emplace<TextElement>(prefix, MessageElementFlag::Text,
                                 MessageColor(QColor(0, 0, 255)),
                                 FontStyle::ChatMediumBold);

    QString headerMessage;
    if (event.lowTrustStatus == lib::suspicious_users::Status::Restricted)
    {
        headerMessage = u"Restricted"_s;
        builder->flags.set(MessageFlag::RestrictedMessage);
    }
    else
    {
        headerMessage = u"Monitored"_s;
        builder->flags.set(MessageFlag::MonitoredMessage);
    }

    auto hasType = [&](lib::suspicious_users::Type type) {
        return std::ranges::find(event.types, type) != event.types.end();
    };

    if (hasType(lib::suspicious_users::Type::BanEvaderDetector))
    {
        QString evader;
        if (event.banEvasionEvaluation ==
            lib::suspicious_users::BanEvasionEvaluation::Likely)
        {
            evader = u"likely"_s;
        }
        else
        {
            evader = u"possible"_s;
        }

        headerMessage += QStringLiteral(". Detected as ") % evader %
                         QStringLiteral(" ban evader");
    }

    if (hasType(lib::suspicious_users::Type::SharedChannelBan))
    {
        headerMessage += QStringLiteral(". Banned in ") %
                         QString::number(event.sharedBanChannelIds.size()) %
                         QStringLiteral(" shared channels");
    }

    builder.emplace<TextElement>(headerMessage, MessageElementFlag::Text,
                                 MessageColor::Text);

    builder.emplace<LinebreakElement>(MessageElementFlag::Text);

    builder
        .emplace<TextElement>(u'#' + event.broadcasterUserLogin.qt(),
                              MessageElementFlag::ChannelName,
                              MessageColor::System)
        ->setLink({Link::JumpToChannel, event.broadcasterUserLogin.qt()});
    builder.emplace<TimestampElement>(time.time());
    builder.emplace<TwitchModerationElement>();

    // sender username
    builder.emplace<MentionElement>(
        event.userName.qt() + ":", event.userLogin.qt(), MessageColor::Text,
        channel->getUserColor(event.userLogin.qt()));

    // sender's message caught by AutoMod
    // XXX: add the structured message here
    builder.emplace<TextElement>(event.message.text.qt(),
                                 MessageElementFlag::Text, MessageColor::Text);

    builder.setMessageAndSearchText(prefix % u" " % headerMessage % u" " %
                                    event.userName.qt() % u": " %
                                    event.message.text.qt());

    QVariantMap tagsMap;
    Communi::TagsRef tags(tagsMap);
    auto highlights =
        builder.parseHighlights(tags, event.message.text.qt(), {}, channel);

    return {builder.release(), highlights};
}

MessagePtr makeSuspiciousUserUpdate(
    TwitchChannel *channel, const QDateTime &time,
    const lib::payload::channel_suspicious_user_update::v1::Event &event)
{
    EventSubMessageBuilder builder(channel, time);
    builder->flags.set(MessageFlag::DoNotTriggerNotification,
                       MessageFlag::ModerationAction);
    builder->loginName = event.moderatorUserLogin.qt();

    QString text;
    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);

    switch (event.lowTrustStatus)
    {
        case lib::suspicious_users::Status::None: {
            builder.emplaceSystemTextAndUpdate(u"removed"_s, text);
            builder.appendUser(event.userName, event.userLogin, text);
            builder.emplaceSystemTextAndUpdate(
                u"from the suspicious user list."_s, text);
        }
        break;

        case lib::suspicious_users::Status::ActiveMonitoring: {
            builder.emplaceSystemTextAndUpdate(u"added"_s, text);
            builder.appendUser(event.userName, event.userLogin, text);
            builder.emplaceSystemTextAndUpdate(
                u"as a monitored suspicious chatter."_s, text);
        }
        break;

        case lib::suspicious_users::Status::Restricted: {
            builder.emplaceSystemTextAndUpdate(u"added"_s, text);
            builder.appendUser(event.userName, event.userLogin, text);
            builder.emplaceSystemTextAndUpdate(
                u"as a restricted suspicious chatter."_s, text);
        }
        break;
    }

    builder.setMessageAndSearchText(text);

    return builder.release();
}

MessagePtr makeUserMessageHeldMessage(
    TwitchChannel *channel, const QDateTime &time,
    const lib::payload::channel_chat_user_message_hold::v1::Event &event)
{
    QString text("AutoMod: Hey! Your message is being checked by mods and has "
                 "not been sent.");
    EventSubMessageBuilder builder(channel);
    builder->timestamp = time;
    builder->id = u"automod_" % event.messageID.qt();
    builder->loginName = u"automod"_s;
    builder->channelName = event.broadcasterUserLogin.qt();
    builder->flags.set(MessageFlag::PubSub, MessageFlag::AutoMod);

    // AutoMod shield badge
    builder.emplace<BadgeElement>(makeAutoModBadge(),
                                  MessageElementFlag::BadgeChannelAuthority);
    // AutoMod "username"
    builder.emplace<TextElement>("AutoMod:", MessageElementFlag::Text,
                                 QColor(0, 0, 255), FontStyle::ChatMediumBold);
    builder.emplace<TextElement>(
        "Hey! Your message is being checked by mods and has not been sent.",
        MessageElementFlag::Text, MessageColor::Text);

    builder.setMessageAndSearchText(text);

    return builder.release();
}

MessagePtr makeUserMessageUpdateMessage(
    TwitchChannel *channel, const QDateTime &time,
    const lib::payload::channel_chat_user_message_update::v1::Event &event)
{
    using lib::payload::channel_chat_user_message_update::v1::Status;

    QString text("AutoMod: ");
    EventSubMessageBuilder builder(channel);
    builder->timestamp = time;
    builder->id = u"automod_" % event.messageID.qt();
    builder->loginName = u"automod"_s;
    builder->channelName = event.broadcasterUserLogin.qt();
    builder->flags.set(MessageFlag::PubSub, MessageFlag::AutoMod);

    // AutoMod shield badge
    builder.emplace<BadgeElement>(makeAutoModBadge(),
                                  MessageElementFlag::BadgeChannelAuthority);
    // AutoMod "username"
    builder.emplace<TextElement>("AutoMod:", MessageElementFlag::Text,
                                 QColor(0, 0, 255), FontStyle::ChatMediumBold);

    switch (event.status)
    {
        case Status::Approved:
            text += "Mods have accepted your message.";
            builder.emplace<TextElement>("Mods have accepted your message.",
                                         MessageElementFlag::Text,
                                         MessageColor::Text);
            break;

        case Status::Denied:
            text += "Mods have denied your message.";
            builder.emplace<TextElement>("Mods have denied your message.",
                                         MessageElementFlag::Text,
                                         MessageColor::Text);
            break;

        case Status::Invalid:
            text += "Your message was lost in the void.";
            builder.emplace<TextElement>("Your message was lost in the void.",
                                         MessageElementFlag::Text,
                                         MessageColor::Text);
            break;
    }

    builder.setMessageAndSearchText(text);

    return builder.release();
}

MessagePtrMut makeFollowMessage(
    TwitchChannel *channel, const QDateTime &time,
    const lib::payload::channel_follow::v2::Event &event)
{
    EventSubMessageBuilder builder(channel, time);
    QString text;
    builder.appendUser(event.userName, event.userLogin, text);
    builder.emplaceSystemTextAndUpdate("followed the channel.", text);
    builder.setMessageAndSearchText(text);
    auto msg = builder.release();
    msg->flags.set(MessageFlag::Follow);
    return msg;
}

}  // namespace chatterino::eventsub

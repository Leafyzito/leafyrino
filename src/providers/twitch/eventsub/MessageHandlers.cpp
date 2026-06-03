// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/eventsub/MessageHandlers.hpp"

#include "Application.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/twitch/eventsub/MessageBuilder.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/FormatTime.hpp"
#include "util/Helpers.hpp"
#include "util/PostToThread.hpp"

namespace chatterino::eventsub {

void handleModerateMessage(
    TwitchChannel *chan, const QDateTime &time,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Clear &)
{
    runInGuiThread([chan, actor{event.moderatorUserLogin.qt()}, time] {
        chan->addOrReplaceClearChat(
            MessageBuilder::makeClearChatMessage(time, actor), time);
        if (getSettings()->hideModerated)
        {
            getApp()->getWindows()->forceLayoutChannelViews();
        }
    });
}

void handleModerateMessage(
    TwitchChannel *chan, const QDateTime &time,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Timeout &action)
{
    std::chrono::system_clock::time_point chronoTime{
        std::chrono::milliseconds{time.toMSecsSinceEpoch()}};

    auto duration =
        std::chrono::round<std::chrono::seconds>(action.expiresAt - chronoTime);

    EventSubMessageBuilder builder(chan, time);
    builder->loginName = event.moderatorUserLogin.qt();

    builder->flags.set(MessageFlag::PubSub, MessageFlag::Timeout,
                       MessageFlag::ModerationAction);

    QString text;
    bool isShared = event.isFromSharedChat();

    builder.emplaceSystemTextAndUpdate(event.moderatorUserLogin.qt(), text)
        ->setLink({Link::UserInfo, event.moderatorUserLogin.qt()});
    builder.emplaceSystemTextAndUpdate("timed out", text);
    builder.emplaceSystemTextAndUpdate(action.userLogin.qt(), text)
        ->setLink({Link::UserInfo, action.userLogin.qt()});

    builder.emplaceSystemTextAndUpdate("for", text);
    builder
        .emplaceSystemTextAndUpdate(
            formatTime(static_cast<int>(duration.count())), text)
        ->setTrailingSpace(isShared);

    if (isShared)
    {
        builder.emplaceSystemTextAndUpdate("in", text);
        builder
            .emplaceSystemTextAndUpdate(event.sourceBroadcasterUserLogin->qt(),
                                        text)
            ->setLink({Link::UserInfo, event.sourceBroadcasterUserLogin->qt()})
            ->setTrailingSpace(false);
        builder->flags.set(MessageFlag::SharedMessage);
        builder->channelName = event.sourceBroadcasterUserLogin->qt();
    }

    assert(text.endsWith(' '));
    removeLastQS(text);

    if (action.reason.view().empty())
    {
        builder.emplaceSystemTextAndUpdate(".", text);
    }
    else
    {
        builder.emplaceSystemTextAndUpdate(":", text);
        builder.emplaceSystemTextAndUpdate(action.reason.qt(), text);
    }

    builder.setMessageAndSearchText(text);
    builder->timeoutUser = action.userLogin.qt();

    auto msg = builder.release();
    runInGuiThread([chan, msg, time] {
        chan->addOrReplaceTimeout(msg, time);
    });
}

void handleModerateMessage(
    TwitchChannel *chan, const QDateTime &time,
    const lib::payload::channel_moderate::v2::Event &event,
    const lib::payload::channel_moderate::v2::Ban &action)
{
    EventSubMessageBuilder builder(chan, time);
    builder->loginName = event.moderatorUserLogin.qt();

    builder->flags.set(MessageFlag::PubSub, MessageFlag::Timeout,
                       MessageFlag::ModerationAction);

    QString text;
    bool isShared = event.isFromSharedChat();

    builder.appendUser(event.moderatorUserName, event.moderatorUserLogin, text);
    builder.emplaceSystemTextAndUpdate("banned", text);
    builder.appendUser(action.userName, action.userLogin, text, isShared);

    if (isShared)
    {
        builder.emplaceSystemTextAndUpdate("in", text);
        builder.appendUser(*event.sourceBroadcasterUserName,
                           *event.sourceBroadcasterUserLogin, text, false);
        builder->flags.set(MessageFlag::SharedMessage);
        builder->channelName = event.sourceBroadcasterUserLogin->qt();
    }

    if (action.reason.view().empty())
    {
        builder.emplaceSystemTextAndUpdate(".", text);
    }
    else
    {
        builder.emplaceSystemTextAndUpdate(":", text);
        builder.emplaceSystemTextAndUpdate(action.reason.qt(), text);
    }

    builder.setMessageAndSearchText(text);
    builder->timeoutUser = action.userLogin.qt();

    auto msg = builder.release();
    runInGuiThread([chan, msg, time] {
        chan->addOrReplaceTimeout(msg, time);
    });
}

void handleModerateMessage(TwitchChannel *chan, const QDateTime &,
                           const lib::payload::channel_moderate::v2::Event &,
                           const lib::payload::channel_moderate::v2::Unraid &)
{
    runInGuiThread([chan] {
        chan->clearActiveRaid();
    });
}

}  // namespace chatterino::eventsub

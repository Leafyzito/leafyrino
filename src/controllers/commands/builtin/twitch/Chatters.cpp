// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/commands/builtin/twitch/Chatters.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/Env.hpp"
#include "common/Literals.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/MultiChannel.hpp"
#include "widgets/ChatterListWidget.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#include <QApplication>
#include <QLoggingCategory>
#include <QString>

#include <algorithm>
#include <ranges>

namespace {

using namespace chatterino;

QString formatChattersError(HelixGetChattersError error, const QString &message)
{
    using Error = HelixGetChattersError;

    QString errorMessage = QString("Failed to get chatter count - ");

    switch (error)
    {
        case Error::Forwarded: {
            errorMessage += message;
        }
        break;

        case Error::UserMissingScope: {
            errorMessage += "Missing required scope. "
                            "Re-login with your "
                            "account and try again.";
        }
        break;

        case Error::UserNotAuthorized: {
            errorMessage += "You must have moderator permissions to "
                            "use this command.";
        }
        break;

        case Error::Unknown: {
            errorMessage += "An unknown error has occurred.";
        }
        break;
    }
    return errorMessage;
}

Split *selectedSplitForChannel(const Channel *channel)
{
    if (!channel)
    {
        return nullptr;
    }
    auto *windows = getApp()->getWindows();
    auto *window = windows ? windows->getLastSelectedWindow() : nullptr;
    auto *page = window ? window->getNotebook().getSelectedPage() : nullptr;
    auto *split = page ? page->getSelectedSplit() : nullptr;
    if (!split)
    {
        return nullptr;
    }
    const auto root = split->getChannel();
    const auto *multi = dynamic_cast<MultiChannel *>(root.get());

    return root.get() == channel ||
                   (multi && std::ranges::any_of(
                                 multi->channels(),
                                 [&](const auto &child) {
                                     return child.channel.get() == channel;
                                 }))
               ? split
               : nullptr;
}

}  // namespace

namespace chatterino::commands {

QString chatters(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    auto *split = selectedSplitForChannel(ctx.channel.get());
    if (!ChatterListWidget::supportsChannel(split ? split->getChannel().get()
                                                  : ctx.channel.get()))
    {
        ctx.channel->addSystemMessage(
            "Chatter lists are only available in Twitch chats.");
        return {};
    }
    if (split)
    {
        split->openChatterList();
        return {};
    }

    ctx.channel->addSystemMessage(
        "Open this chat in a split before using /chatters.");
    return {};
}

QString testChatters(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "The /test-chatters command only works in Twitch Channels.");
        return "";
    }

    getHelix()->getChatters(
        ctx.twitchChannel->roomId(),
        getApp()->getAccounts()->twitch.getCurrent()->getUserId(), 5000,
        nullptr,
        [channel{ctx.channel}, twitchChannel{ctx.twitchChannel}](auto result) {
            QStringList entries;
            for (const auto &username : result.chatters)
            {
                entries << username;
            }

            QString prefix = "Chatters ";

            if (result.total > 5000)
            {
                prefix += QString("(5000/%1):").arg(result.total);
            }
            else
            {
                prefix += QString("(%1):").arg(result.total);
            }

            channel->addMessage(MessageBuilder::makeListOfUsersMessage(
                                    prefix, entries, twitchChannel),
                                MessageContext::Original);
        },
        [channel{ctx.channel}](auto error, auto message) {
            auto errorMessage = formatChattersError(error, message);
            channel->addSystemMessage(errorMessage);
        });

    return "";
}

}  // namespace chatterino::commands

// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/commands/builtin/twitch/UpdateChannel.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "providers/twitch/ChannelManagement.hpp"
#include "providers/twitch/TwitchChannel.hpp"

#include <boost/signals2/connection.hpp>

#include <memory>

namespace chatterino::commands {

QString setTitle(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage("Usage: /settitle <stream title>");
        return "";
    }

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "Unable to set title of non-Twitch channel.");
        return "";
    }

    const auto title = ctx.words.mid(1).join(" ").trimmed();
    auto twitchChannel = std::dynamic_pointer_cast<TwitchChannel>(ctx.channel);

    ChannelManagement::updateTitle(
        twitchChannel, title,
        [channel{ctx.channel}, title] {
            channel->addSystemMessage(
                QString("Updated title to %1").arg(title));
        },
        [channel{ctx.channel}](const QString &error) {
            channel->addSystemMessage(error);
        });

    return "";
}

QString setGame(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage("Usage: /setgame <stream game>");
        return "";
    }

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "Unable to set game of non-Twitch channel.");
        return "";
    }

    const auto gameName = ctx.words.mid(1).join(" ");

    auto twitchChannel = std::dynamic_pointer_cast<TwitchChannel>(ctx.channel);
    auto accountChanged = std::make_shared<bool>(false);
    auto connection = std::make_shared<boost::signals2::scoped_connection>(
        getApp()->getAccounts()->twitch.currentUserChanged.connect(
            [accountChanged] {
                *accountChanged = true;
            }));

    ChannelManagement::searchCategories(
        gameName,
        [channel{ctx.channel}, twitchChannel, gameName, accountChanged,
         connection](const std::vector<ChannelManagementCategory> &games) {
            if (*accountChanged)
            {
                channel->addSystemMessage("The Twitch account changed while "
                                          "looking up the category.");
                return;
            }
            if (games.empty())
            {
                channel->addSystemMessage("Game not found.");
                return;
            }

            auto matchedGame = games.at(0);

            if (games.size() > 1)
            {
                for (const auto &game : games)
                {
                    if (game.name.toLower() == gameName.toLower())
                    {
                        matchedGame = game;
                        break;
                    }
                }
            }

            ChannelManagement::updateCategory(
                twitchChannel, matchedGame,
                [channel, matchedGame] {
                    channel->addSystemMessage(
                        QString("Updated game to %1").arg(matchedGame.name));
                },
                [channel](const QString &error) {
                    channel->addSystemMessage(error);
                });
        },
        [channel{ctx.channel}](const QString &error) {
            channel->addSystemMessage(error);
        });

    return "";
}

}  // namespace chatterino::commands

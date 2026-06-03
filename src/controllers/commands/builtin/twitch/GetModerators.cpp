// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/commands/builtin/twitch/GetModerators.hpp"

#include "common/Channel.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/IvrApi.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchChannel.hpp"

namespace {

using namespace chatterino;

QString targetChannelLogin(const CommandContext &ctx)
{
    if (ctx.words.size() > 1)
    {
        auto login = ctx.words.at(1).trimmed();
        if (login.startsWith('#'))
        {
            login.remove(0, 1);
        }
        return login.toLower();
    }

    if (ctx.twitchChannel != nullptr)
    {
        return ctx.twitchChannel->getName().toLower();
    }

    return {};
}

QString formatModsError(HelixGetModeratorsError error, const QString &message)
{
    using Error = HelixGetModeratorsError;

    QString errorMessage = QString("Failed to get moderators - ");

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
            errorMessage +=
                "Due to Twitch restrictions, "
                "this command can only be used by the broadcaster. "
                "To see the list of mods you must use the Twitch website.";
        }
        break;

        case Error::Unknown: {
            errorMessage += "An unknown error has occurred.";
        }
        break;
    }
    return errorMessage;
}

}

namespace chatterino::commands {

QString getModerators(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    const auto channelLogin = targetChannelLogin(ctx);
    if (channelLogin.isEmpty())
    {
        ctx.channel->addSystemMessage(
            "The /mods command only works in Twitch Channels.");
        return "";
    }

    if (ctx.words.size() <= 1 && ctx.twitchChannel != nullptr &&
        ctx.twitchChannel->isBroadcaster())
    {
        getHelix()->getModerators(
            ctx.twitchChannel->roomId(), 500,
            [channel{ctx.channel}, twitchChannel{ctx.twitchChannel}](
                auto result) {
                if (result.empty())
                {
                    channel->addSystemMessage(
                        "This channel does not have any moderators.");
                    return;
                }

                channel->addMessage(MessageBuilder::makeListOfUsersMessage(
                                        "The moderators of this channel are",
                                        result, twitchChannel),
                                    MessageContext::Original);
            },
            [channel{ctx.channel}](auto error, auto message) {
                auto errorMessage = formatModsError(error, message);
                channel->addSystemMessage(errorMessage);
            });

        return "";
    }

    getIvr()->getModVip(
        channelLogin,
        [channel{ctx.channel}](
            const std::vector<HelixModerator> &mods,
            const std::vector<HelixVip> &) {
            if (mods.empty())
            {
                channel->addSystemMessage("This channel does not have any moderators.");
                return;
            }

            channel->addMessage(MessageBuilder::makeListOfUsersMessage(
                                    "The moderators of this channel are",
                                    mods, channel.get()),
                                MessageContext::Original);
        },
        [channel{ctx.channel}] {
            channel->addSystemMessage("Could not get moderator list!");
        });

    return "";
}

}

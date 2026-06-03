// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/commands/builtin/twitch/GetFounders.hpp"

#include "controllers/commands/CommandContext.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/IvrApi.hpp"
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

}  // namespace

namespace chatterino::commands {

QString getFounders(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    const auto channelLogin = targetChannelLogin(ctx);
    if (channelLogin.isEmpty())
    {
        ctx.channel->addSystemMessage(
            "The /founders command only works in Twitch channels.");
        return "";
    }

    getIvr()->getFounders(
        channelLogin,
        [channel{ctx.channel}](const std::vector<HelixModerator> &founders) {
            if (founders.empty())
            {
                channel->addSystemMessage(
                    "This channel does not have any founders.");
                return;
            }

            channel->addMessage(MessageBuilder::makeListOfUsersMessage(
                                    "The founders of this channel are",
                                    founders, channel.get()),
                                MessageContext::Original);
        },
        [channel{ctx.channel}] {
            channel->addSystemMessage("Could not get founders list!");
        });

    return "";
}

}  // namespace chatterino::commands

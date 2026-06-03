// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/commands/builtin/twitch/GetVIPs.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/IvrApi.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchAccount.hpp"
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

QString formatGetVIPsError(HelixListVIPsError error, const QString &message)
{
    using Error = HelixListVIPsError;

    QString errorMessage = QString("Failed to list VIPs - ");

    switch (error)
    {
        case Error::Forwarded: {
            errorMessage += message;
        }
        break;

        case Error::Ratelimited: {
            errorMessage += "You are being ratelimited by Twitch. Try "
                            "again in a few seconds.";
        }
        break;

        case Error::UserMissingScope: {

            errorMessage += "Missing required scope. "
                            "Re-login with your "
                            "account and try again.";
        }
        break;

        case Error::UserNotAuthorized: {

            errorMessage += "You don't have permission to "
                            "perform that action.";
        }
        break;

        case Error::UserNotBroadcaster: {
            errorMessage +=
                "Due to Twitch restrictions, "
                "this command can only be used by the broadcaster. "
                "To see the list of VIPs you must use the Twitch website.";
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

QString getVIPs(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    const auto channelLogin = targetChannelLogin(ctx);
    if (channelLogin.isEmpty())
    {
        ctx.channel->addSystemMessage(
            "The /vips command only works in Twitch channels.");
        return "";
    }

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (ctx.words.size() <= 1 && ctx.twitchChannel != nullptr &&
        ctx.twitchChannel->isBroadcaster() &&
        !currentUser->isAnon())
    {
        getHelix()->getChannelVIPs(
            ctx.twitchChannel->roomId(),
            [channel{ctx.channel}, twitchChannel{ctx.twitchChannel}](
                const std::vector<HelixVip> &vipList) {
                if (vipList.empty())
                {
                    channel->addSystemMessage(
                        "This channel does not have any VIPs.");
                    return;
                }

                auto messagePrefix =
                    QString("The VIPs (%1) of this channel are")
                        .arg(vipList.size());

                // TODO: sort results?

                channel->addMessage(MessageBuilder::makeListOfUsersMessage(
                                        messagePrefix, vipList, twitchChannel),
                                    MessageContext::Original);
            },
            [channel{ctx.channel}](auto error, auto message) {
                auto errorMessage = formatGetVIPsError(error, message);
                channel->addSystemMessage(errorMessage);
            });
        return "";
    }

    getIvr()->getModVip(
        channelLogin,
        [channel{ctx.channel}, twitchChannel{ctx.twitchChannel}](
            const std::vector<HelixModerator> &,
            const std::vector<HelixVip> &vips) {
            if (vips.empty())
            {
                channel->addSystemMessage(
                    "This channel does not have any VIPs.");
                return;
            }

            auto messagePrefix =
                QString("The VIPs (%1) of this channel are").arg(vips.size());
            channel->addMessage(MessageBuilder::makeListOfUsersMessage(
                                    messagePrefix, vips,
                                    twitchChannel != nullptr ? twitchChannel
                                                            : channel.get()),
                                MessageContext::Original);
        },
        [channel{ctx.channel}] {
            channel->addSystemMessage("Could not get VIP list!");
        });

    return "";
}

}

// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/commands/builtin/twitch/StartCommercial.hpp"

#include "controllers/commands/CommandContext.hpp"
#include "providers/twitch/ChannelManagement.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"

namespace chatterino::commands {

QString startCommercial(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "The /commercial command only works in Twitch channels.");
        return "";
    }

    const auto *usageStr =
        "Usage: /commercial [seconds]. Choose 30, 60, 90, 120, 150, or 180. "
        "Without a duration, uses the default in Settings > Moltorino > "
        "Moderation.";

    auto length = getSettings()->defaultCommercialDuration.getValue();
    if (!ChannelManagement::isValidCommercialLength(length))
    {
        length = 30;
    }
    if (ctx.words.size() >= 2)
    {
        bool parsedLength = false;
        length = ctx.words.at(1).toInt(&parsedLength);
        if (!parsedLength ||
            !ChannelManagement::isValidCommercialLength(length))
        {
            ctx.channel->addSystemMessage(usageStr);
            return "";
        }
    }

    auto twitchChannel = std::dynamic_pointer_cast<TwitchChannel>(ctx.channel);

    ChannelManagement::startCommercial(
        twitchChannel, length, ChannelManagementCommercialTrigger::ChatCommand,
        [channel{ctx.channel}](ChannelManagementCommercialResult result) {
            auto message =
                QString("Starting %1 second long commercial break. Keep in "
                        "mind you are still live and not all viewers will "
                        "receive a commercial.")
                    .arg(result.lengthSeconds);
            if (result.retryAfterSeconds > 0)
            {
                message +=
                    QString(" You may run another commercial in %1 seconds.")
                        .arg(result.retryAfterSeconds);
            }
            channel->addSystemMessage(message);
        },
        [channel{ctx.channel}](ChannelManagementCommercialFailure failure) {
            channel->addSystemMessage(failure.message);
        });

    return "";
}

}  // namespace chatterino::commands

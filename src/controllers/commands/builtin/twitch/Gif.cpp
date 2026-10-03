// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/commands/builtin/twitch/Gif.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/dialogs/GifPickerDialog.hpp"
#include "widgets/Window.hpp"

namespace chatterino::commands {

QString openGifPicker(const CommandContext &ctx)
{
    if (ctx.twitchChannel == nullptr)
    {
        if (ctx.channel != nullptr)
        {
            ctx.channel->addSystemMessage(
                "The /gif command only works in Twitch channels.");
        }
        return {};
    }

    // Everything after the command is what to search for.
    const auto searchTerm = ctx.words.mid(1).join(' ').trimmed();
    GifPickerDialog::showDialog(
        ctx.twitchChannel, searchTerm,
        getApp()->getWindows()->getLastSelectedWindow());
    return {};
}

}  // namespace chatterino::commands

// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

namespace chatterino {

struct CommandContext;

}  // namespace chatterino

namespace chatterino::commands {

/// /gif [search term]: opens the GIF picker for the channel.
QString openGifPicker(const CommandContext &ctx);

}  // namespace chatterino::commands

#pragma once

class QString;

namespace chatterino {

struct CommandContext;

}  // namespace chatterino

namespace chatterino::commands {

/// /gif [search term]: opens the GIF picker for the channel.
QString openGifPicker(const CommandContext &ctx);

}  // namespace chatterino::commands

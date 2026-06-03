#pragma once

class QString;

namespace chatterino {

struct CommandContext;

}

namespace chatterino::commands {

QString doKickBan(const CommandContext &ctx);

QString doKickTimeout(const CommandContext &ctx);

QString doKickUnban(const CommandContext &ctx);

QString doKickDelete(const CommandContext &ctx);

}  // namespace chatterino::commands

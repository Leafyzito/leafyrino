#pragma once

#include <QString>

namespace chatterino {

struct CommandContext;

}

namespace chatterino::commands {

QString startRaid(const CommandContext &ctx);

QString cancelRaid(const CommandContext &ctx);

QString sendRaidNow(const CommandContext &ctx);

}  // namespace chatterino::commands

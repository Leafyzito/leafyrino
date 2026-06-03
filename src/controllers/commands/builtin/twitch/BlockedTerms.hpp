#pragma once

class QString;

namespace chatterino {

struct CommandContext;

}

namespace chatterino::commands {

QString blockTerm(const CommandContext &ctx);

QString unblockTerm(const CommandContext &ctx);

}  // namespace chatterino::commands

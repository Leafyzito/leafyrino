#pragma once

class QString;

namespace chatterino {

struct CommandContext;

}

namespace chatterino::commands {

QString createPoll(const CommandContext &ctx);

QString endPoll(const CommandContext &ctx);

QString cancelPoll(const CommandContext &ctx);

}

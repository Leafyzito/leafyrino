#pragma once

class QString;

namespace chatterino {

struct CommandContext;

}

namespace chatterino::commands {

QString debugKickRawEvent(const CommandContext &ctx);

}

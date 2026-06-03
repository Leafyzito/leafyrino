#pragma once

class QString;

namespace chatterino {

struct CommandContext;

}

namespace chatterino::commands {

QString removeModerator(const CommandContext &ctx);

}

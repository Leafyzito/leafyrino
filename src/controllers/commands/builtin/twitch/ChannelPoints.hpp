#pragma once

class QString;

namespace chatterino {

struct CommandContext;

}  // namespace chatterino

namespace chatterino::commands {

QString openChannelPointRewards(const CommandContext &ctx);
QString openChannelPointsChart(const CommandContext &ctx);

}  // namespace chatterino::commands

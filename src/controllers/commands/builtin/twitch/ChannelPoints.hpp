#pragma once

class QString;

namespace chatterino {

struct CommandContext;

}  // namespace chatterino

namespace chatterino::commands {

QString openChannelPointRewards(const CommandContext &ctx);
QString openChannelPointsChart(const CommandContext &ctx);
QString openRewardQueue(const CommandContext &ctx);
QString sendGigantifiedEmote(const CommandContext &ctx);

}  // namespace chatterino::commands

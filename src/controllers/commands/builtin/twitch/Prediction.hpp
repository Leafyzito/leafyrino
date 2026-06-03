#pragma once

class QString;

namespace chatterino {

struct CommandContext;

}

namespace chatterino::commands {

QString createPrediction(const CommandContext &ctx);

QString lockPrediction(const CommandContext &ctx);

QString cancelPrediction(const CommandContext &ctx);

QString completePrediction(const CommandContext &ctx);

QString showPredictions(const CommandContext &ctx);

}

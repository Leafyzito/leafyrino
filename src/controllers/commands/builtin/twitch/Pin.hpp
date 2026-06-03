#pragma once

#include <QString>

namespace chatterino {

struct CommandContext;

}

namespace chatterino::commands {

struct PinDurationParseResult {
    bool matched = false;
    QString error;
    int durationSeconds = 0;
};

PinDurationParseResult parsePinDuration(const QString &text);

int normalizePinDuration(int durationSeconds);

QString pinMessage(const CommandContext &ctx);

QString unpinMessage(const CommandContext &ctx);

}

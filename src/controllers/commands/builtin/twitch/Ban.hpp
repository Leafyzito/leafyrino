// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

namespace chatterino {

struct CommandContext;

}

namespace chatterino::commands {

QString sendBan(const CommandContext &ctx);

QString sendBanById(const CommandContext &ctx);

QString sendTimeout(const CommandContext &ctx);

}

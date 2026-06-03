// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

class QString;

namespace chatterino {

struct CommandContext;

}

namespace chatterino::commands {

QString monitorUser(const CommandContext &ctx);

QString restrictUser(const CommandContext &ctx);

QString unmonitorUser(const CommandContext &ctx);

QString unrestrictUser(const CommandContext &ctx);

}

// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

class QString;

namespace chatterino {

struct CommandContext;

}

namespace chatterino::commands {

QString blockUser(const CommandContext &ctx);

QString ignoreUser(const CommandContext &ctx);

QString unblockUser(const CommandContext &ctx);

QString unignoreUser(const CommandContext &ctx);

}

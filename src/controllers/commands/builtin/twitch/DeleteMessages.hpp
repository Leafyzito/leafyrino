// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

class QString;

namespace chatterino {

struct CommandContext;

}

namespace chatterino::commands {

QString deleteAllMessages(const CommandContext &ctx);

QString deleteOneMessage(const CommandContext &ctx);

}  // namespace chatterino::commands

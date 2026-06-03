// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

class QString;

namespace chatterino {

struct CommandContext;

}

namespace chatterino::commands {

QString sendAnnouncement(const CommandContext &ctx);

QString sendAnnouncementBlue(const CommandContext &ctx);

QString sendAnnouncementGreen(const CommandContext &ctx);

QString sendAnnouncementOrange(const CommandContext &ctx);

QString sendAnnouncementPurple(const CommandContext &ctx);

}

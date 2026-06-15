// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/buttons/SvgButton.hpp"

class QString;

namespace chatterino {

class TwitchChannel;

SvgButton::Src followButtonSource(bool following);

bool canUseFollowButtonForUser(const QString &userId, const QString &login);

bool canUseFollowButtonForChannel(const TwitchChannel &channel);

}  // namespace chatterino

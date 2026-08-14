// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <optional>

namespace chatterino {

extern const QStringList VALID_HELIX_COLORS;

void openTwitchUsercard(const QString channel, const QString username);

void stripUserName(QString &userName);

void stripChannelName(QString &channelName);

QString cleanChannelName(const QString &dirtyChannelName);

using ParsedUserName = QString;
using ParsedUserID = QString;

std::pair<ParsedUserName, ParsedUserID> parseUserNameOrID(const QString &input);

QRegularExpression twitchUserLoginRegexp();

QRegularExpression twitchUserNameRegexp();

void cleanHelixColorName(QString &color);

QString helixColorDisplayHex(const QString &helixColorName);

QString formatHelixColorLabel(const QString &helixColorName);

std::optional<QString> helixColorNameFromDisplayHex(const QString &hex);

class TwitchChannel;

/// Build a Chat Vault badge URL for a Twitch badge set/version.
/// When \a twitchChannel is provided, channel-specific badges include the
/// room ID in the path (matching Chat Vault's URL scheme).
std::optional<QString> chatVaultBadgeUrl(
    const QString &setID, const QString &version,
    const TwitchChannel *twitchChannel = nullptr);

}  // namespace chatterino

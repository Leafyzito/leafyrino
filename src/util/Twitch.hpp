// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringList>

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

}

// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QDateTime>
#include <QString>

#include <chrono>

namespace chatterino {

QString formatTime(int totalSeconds, int components = 4);
QString formatTime(const QString &totalSecondsString, int components = 4);
QString formatTime(std::chrono::seconds totalSeconds, int components = 4);

QString formatDurationExact(std::chrono::seconds seconds);

QString formatLongFriendlyDuration(const QDateTime &from, const QDateTime &to);

}  // namespace chatterino

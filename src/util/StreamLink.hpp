// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>
#include <QStringList>

#include <stdexcept>
#include <string>

namespace chatterino {

class Exception : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

#ifdef Q_OS_WIN
constexpr inline QStringView STREAMLINK_BINARY_NAME = u"streamlink.exe";
#else
constexpr inline QStringView STREAMLINK_BINARY_NAME = u"streamlink";
#endif

void openStreamlink(const QString &channelURL, const QString &quality,
                    QStringList extraArguments = QStringList());

void openStreamlinkForChannel(const QString &channel,
                              QStringView prefixURL = u"twitch.tv/");

}

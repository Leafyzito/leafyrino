// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QDir>
#include <QString>

namespace chatterino {

inline QString combinePath(const QString &a, const QString &b)
{
    return QDir::cleanPath(a + QDir::separator() + b);
}

}  // namespace chatterino

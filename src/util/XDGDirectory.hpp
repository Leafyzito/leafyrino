// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QStringList>

#include <cstdint>

namespace chatterino {

#if defined(Q_OS_UNIX) and !defined(Q_OS_DARWIN)

enum class XDGDirectoryType : std::uint8_t {

    Config,

    Data,
};

QStringList getXDGBaseDirectories(XDGDirectoryType directory);

QStringList getXDGUserDirectories(XDGDirectoryType directory);

QStringList getXDGDirectories(XDGDirectoryType directory);

#endif

}  // namespace chatterino

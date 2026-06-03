// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/XDGDesktopFile.hpp"

#include <QString>

namespace chatterino {

#if defined(Q_OS_UNIX) and !defined(Q_OS_DARWIN)

std::optional<XDGDesktopFile> getDefaultBrowserDesktopFile();

QString parseDesktopExecProgram(const QString &execKey);

#endif

}

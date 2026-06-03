// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/QStringHash.hpp"

#include <optional>
#include <unordered_map>

#if defined(Q_OS_UNIX) and !defined(Q_OS_DARWIN)

namespace chatterino {

using XDGEntries = std::unordered_map<QString, QString>;

class XDGDesktopFile
{
public:

    explicit XDGDesktopFile(const QString &filename);

    XDGEntries getEntries(const QString &groupHeader) const;

    bool isValid() const
    {
        return this->valid;
    }

    static std::optional<XDGDesktopFile> findDesktopFile(
        const QString &desktopFileID);

private:
    bool valid{};
    std::unordered_map<QString, XDGEntries> groups;
};

}

#endif

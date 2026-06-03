// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/XDGDesktopFile.hpp"

#include "util/XDGDirectory.hpp"

#include <QDir>
#include <QFile>

#include <functional>

#if defined(Q_OS_UNIX) and !defined(Q_OS_DARWIN)

namespace chatterino {

XDGDesktopFile::XDGDesktopFile(const QString &filename)
{
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly))
    {
        return;
    }
    this->valid = true;

    std::optional<std::reference_wrapper<XDGEntries>> entries;

    while (!file.atEnd())
    {
        auto lineBytes = file.readLine().trimmed();

        if (lineBytes.startsWith('#') || lineBytes.size() == 0)
        {
            continue;
        }

        auto line = QString::fromUtf8(lineBytes);

        if (line.startsWith('['))
        {
            auto end = line.indexOf(']', 1);
            if (end == -1 || end == 1)
            {
                continue;
            }
            auto groupName = line.mid(1, end - 1);

            entries = this->groups[groupName];

            continue;
        }

        if (!entries.has_value())
        {
            continue;
        }

        auto delimiter = line.indexOf('=');
        if (delimiter == -1)
        {
            continue;
        }

        auto key = QStringView(line).left(delimiter).trimmed().toString();

        auto valueStart = delimiter + 1;
        QString value;
        if (valueStart < line.size())
        {
            value = QStringView(line).mid(valueStart).trimmed().toString();
        }

        entries->get().emplace(key, value);
    }
}

XDGEntries XDGDesktopFile::getEntries(const QString &groupHeader) const
{
    auto group = this->groups.find(groupHeader);
    if (group != this->groups.end())
    {
        return group->second;
    }

    return {};
}

std::optional<XDGDesktopFile> XDGDesktopFile::findDesktopFile(
    const QString &desktopFileID)
{
    for (const auto &dataDir : getXDGDirectories(XDGDirectoryType::Data))
    {
        auto fileName =
            QDir::cleanPath(dataDir + QDir::separator() + "applications" +
                            QDir::separator() + desktopFileID);
        XDGDesktopFile desktopFile(fileName);
        if (desktopFile.isValid())
        {
            return desktopFile;
        }
    }
    return {};
}

}  // namespace chatterino

#endif

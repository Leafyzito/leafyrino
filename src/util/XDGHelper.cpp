// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/XDGHelper.hpp"

#include "common/Literals.hpp"
#include "common/QLogging.hpp"
#include "util/CombinePath.hpp"
#include "util/XDGDesktopFile.hpp"
#include "util/XDGDirectory.hpp"

#include <QDebug>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStringLiteral>
#include <QtGlobal>

#include <unordered_set>

#if defined(Q_OS_UNIX) and !defined(Q_OS_DARWIN)

using namespace chatterino::literals;

namespace {

const auto &LOG = chatterinoXDG;

using namespace chatterino;

const auto HTTPS_MIMETYPE = u"x-scheme-handler/https"_s;

std::optional<XDGDesktopFile> processMimeAppsList(
    const QString &mimeappsPath, QStringList &associations,
    std::unordered_set<QString> &denyList)
{
    XDGDesktopFile mimeappsFile(mimeappsPath);
    if (!mimeappsFile.isValid())
    {
        return {};
    }

    auto defaultGroup = mimeappsFile.getEntries("Default Applications");
    auto defaultApps = defaultGroup.find(HTTPS_MIMETYPE);
    if (defaultApps != defaultGroup.cend())
    {

        auto desktopIds = defaultApps->second.split(';', Qt::SkipEmptyParts);
        for (const auto &entry : desktopIds)
        {
            auto desktopId = entry.trimmed();

            if (!denyList.contains(desktopId))
            {
                auto desktopFile = XDGDesktopFile::findDesktopFile(desktopId);

                if (desktopFile.has_value())
                {
                    return desktopFile;
                }
            }
        }
    }

    auto removedGroup = mimeappsFile.getEntries("Removed Associations");
    auto removedApps = removedGroup.find(HTTPS_MIMETYPE);
    if (removedApps != removedGroup.end())
    {
        auto desktopIds = removedApps->second.split(';', Qt::SkipEmptyParts);
        for (const auto &entry : desktopIds)
        {
            denyList.insert(entry.trimmed());
        }
    }

    auto addedGroup = mimeappsFile.getEntries("Added Associations");
    auto addedApps = addedGroup.find(HTTPS_MIMETYPE);
    if (addedApps != addedGroup.end())
    {
        auto desktopIds = addedApps->second.split(';', Qt::SkipEmptyParts);
        for (const auto &entry : desktopIds)
        {
            associations.push_back(entry.trimmed());
        }
    }

    return {};
}

std::optional<XDGDesktopFile> searchMimeAppsListsInDirectory(
    const QString &directory, QStringList &associations,
    std::unordered_set<QString> &denyList)
{
    static auto desktopNames = qEnvironmentVariable("XDG_CURRENT_DESKTOP")
                                   .split(':', Qt::SkipEmptyParts);
    static const QString desktopFilename = QStringLiteral("%1-mimeapps.list");
    static const QString nonDesktopFilename = QStringLiteral("mimeapps.list");

    for (const auto &desktopName : desktopNames)
    {
        auto fileName =
            combinePath(directory, desktopFilename.arg(desktopName));
        auto defaultApp = processMimeAppsList(fileName, associations, denyList);
        if (defaultApp.has_value())
        {
            return defaultApp;
        }
    }

    auto fileName = combinePath(directory, nonDesktopFilename);
    auto defaultApp = processMimeAppsList(fileName, associations, denyList);
    if (defaultApp.has_value())
    {
        return defaultApp;
    }

    return {};
}

}

namespace chatterino {

std::optional<XDGDesktopFile> getDefaultBrowserDesktopFile()
{

    QStringList associations;
    std::unordered_set<QString> denyList;

    for (const auto &configDir : getXDGDirectories(XDGDirectoryType::Config))
    {
        auto defaultApp =
            searchMimeAppsListsInDirectory(configDir, associations, denyList);
        if (defaultApp.has_value())
        {
            return defaultApp;
        }
    }

    for (const auto &dataDir : getXDGDirectories(XDGDirectoryType::Data))
    {
        auto appsDir = combinePath(dataDir, "applications");
        auto defaultApp =
            searchMimeAppsListsInDirectory(appsDir, associations, denyList);
        if (defaultApp.has_value())
        {
            return defaultApp;
        }
    }

    if (!associations.empty())
    {
        for (const auto &desktopId : associations)
        {
            auto desktopFile = XDGDesktopFile::findDesktopFile(desktopId);
            if (desktopFile.has_value())
            {
                return desktopFile;
            }
        }
    }

    QProcess xdgSettings;
    xdgSettings.start("xdg-settings", {"get", "default-web-browser"},
                      QIODevice::ReadOnly);
    xdgSettings.waitForFinished(1000);
    if (xdgSettings.exitStatus() == QProcess::ExitStatus::NormalExit &&
        xdgSettings.error() == QProcess::UnknownError &&
        xdgSettings.exitCode() == 0)
    {
        return XDGDesktopFile::findDesktopFile(
            xdgSettings.readAllStandardOutput().trimmed());
    }

    return {};
}

QString parseDesktopExecProgram(const QString &execKey)
{
    static const QRegularExpression unescapeReservedCharacters(
        R"(\\(["`$\\]))");

    QString program = execKey;

    program.replace(u"\\\\"_s, u"\\"_s);

    if (!program.startsWith('"'))
    {

        auto end = program.indexOf(' ');
        if (end != -1)
        {
            program = program.left(end);
        }
    }
    else
    {

        auto endQuote = program.indexOf('"', 1);
        if (endQuote == -1)
        {

            program = program.mid(1);
            qCWarning(LOG).noquote().nospace()
                << "Malformed desktop entry key " << program << ", originally "
                << execKey << ", you might run into issues";
        }
        else
        {

            program = program.mid(1, endQuote - 1);
        }
    }

    program.replace(unescapeReservedCharacters, "\\1");

    return program;
}

}

#endif

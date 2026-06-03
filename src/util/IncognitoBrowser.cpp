// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/IncognitoBrowser.hpp"
#ifdef USEWINSDK
#    include "util/WindowsHelper.hpp"
#elif defined(Q_OS_UNIX) and !defined(Q_OS_DARWIN)
#    include "util/XDGHelper.hpp"
#endif

#include <QFileInfo>
#include <QProcess>
#include <QVariant>

namespace {

using namespace chatterino;

QString getDefaultBrowserExecutable()
{
#ifdef USEWINSDK

    QString command =
        getAssociatedExecutable(AssociationQueryType::Protocol, L"http");

    if (command.isNull())
    {

        command = getAssociatedExecutable(AssociationQueryType::FileExtension,
                                          L".html");
    }

    if (command.isNull())
    {

        command = getAssociatedExecutable(AssociationQueryType::FileExtension,
                                          L".htm");
    }

    return command;
#elif defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
    static QString defaultBrowser = []() -> QString {
        auto desktopFile = getDefaultBrowserDesktopFile();
        if (desktopFile.has_value())
        {
            auto entry = desktopFile->getEntries("Desktop Entry");
            auto exec = entry.find("Exec");
            if (exec != entry.end())
            {
                return parseDesktopExecProgram(exec->second.trimmed());
            }
        }
        return {};
    }();

    return defaultBrowser;
#else
    return {};
#endif
}

}

namespace chatterino::incognitobrowser::detail {

QString getPrivateSwitch(const QString &browserExecutable)
{
    static auto switches = std::vector<std::pair<QString, QString>>{
        {"librewolf", "-private-window"},
        {"waterfox", "-private-window"},
        {"icecat", "-private-window"},
        {"chrome", "-incognito"},
        {"google-chrome-stable", "-incognito"},
        {"vivaldi", "-incognito"},
        {"opera", "-newprivatetab"},
        {"msedge", "-inprivate"},
        {"chromium", "-incognito"},
        {"brave", "-incognito"},
    };

    auto lowercasedBrowserExecutable =
        QFileInfo(browserExecutable).baseName().toLower();

#ifdef Q_OS_WINDOWS
    if (lowercasedBrowserExecutable.endsWith(".exe"))
    {
        lowercasedBrowserExecutable.chop(4);
    }
#endif

    for (const auto &switch_ : switches)
    {
        if (lowercasedBrowserExecutable == switch_.first)
        {
            return switch_.second;
        }
    }

    if (lowercasedBrowserExecutable.startsWith("firefox"))
    {
        return "-private-window";
    }

    return {};
}

}

namespace chatterino {

using namespace chatterino::incognitobrowser::detail;

bool supportsIncognitoLinks()
{
    auto browserExe = getDefaultBrowserExecutable();
    return !browserExe.isNull() && !getPrivateSwitch(browserExe).isNull();
}

bool openLinkIncognito(const QString &link)
{
    auto browserExe = getDefaultBrowserExecutable();
    return QProcess::startDetached(browserExe,
                                   {getPrivateSwitch(browserExe), link});
}

}

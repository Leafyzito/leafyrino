// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/WindowsHelper.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"

#include <QApplication>
#include <QClipboard>
#include <QFileInfo>
#include <QSettings>

#ifdef USEWINSDK

#    include <Ole2.h>
#    include <ShellScalingApi.h>
#    include <Shlwapi.h>
#    include <VersionHelpers.h>

namespace chatterino {

using namespace literals;

using GetDpiForMonitor_ = HRESULT(CALLBACK *)(HMONITOR, MONITOR_DPI_TYPE,
                                              UINT *, UINT *);

std::optional<UINT> getWindowDpi(HWND hwnd)
{
    static HINSTANCE shcore = LoadLibrary(L"Shcore.dll");
    if (shcore != nullptr)
    {
        if (auto getDpiForMonitor =
                GetDpiForMonitor_(GetProcAddress(shcore, "GetDpiForMonitor")))
        {
            HMONITOR monitor =
                MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);

            UINT xScale = 96;
            UINT yScale = 96;
            getDpiForMonitor(monitor, MDT_DEFAULT, &xScale, &yScale);

            return xScale;
        }
    }

    return std::nullopt;
}

void flushClipboard()
{
    if (QApplication::clipboard()->ownsClipboard())
    {
        OleFlushClipboard();
    }
}

const QString RUN_KEY =
    uR"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run)"_s;

bool isRegisteredForStartup()
{
    QSettings settings(RUN_KEY, QSettings::NativeFormat);

    return !settings.value("Chatterino").toString().isEmpty();
}

void setRegisteredForStartup(bool isRegistered)
{
    auto *app = tryGetApp();
    if (app && app->isTest())
    {
        return;
    }

    QSettings settings(RUN_KEY, QSettings::NativeFormat);

    if (isRegistered)
    {
        auto exePath = QFileInfo(QCoreApplication::applicationFilePath())
                           .absoluteFilePath()
                           .replace('/', '\\');

        settings.setValue("Chatterino", "\"" + exePath + "\" --autorun");
    }
    else
    {
        settings.remove("Chatterino");
    }
}

QString getAssociatedExecutable(AssociationQueryType queryType, LPCWSTR query)
{
    ASSOCF flags = ASSOCF_NOTRUNCATE;

    if (queryType == AssociationQueryType::Protocol)
    {
        if (IsWindows8OrGreater())
        {
            flags |= ASSOCF_IS_PROTOCOL;
        }
        else
        {
            return {};
        }
    }

    DWORD resultSize = 0;
    if (FAILED(AssocQueryStringW(flags, ASSOCSTR_EXECUTABLE, query, nullptr,
                                 nullptr, &resultSize)))
    {
        return {};
    }

    if (resultSize <= 1)
    {
        return {};
    }

    QString result;
    auto *buf = new wchar_t[resultSize];
    if (SUCCEEDED(AssocQueryStringW(flags, ASSOCSTR_EXECUTABLE, query, nullptr,
                                    buf, &resultSize)))
    {
        result = QString::fromWCharArray(buf, resultSize - 1);
    }
    delete[] buf;
    return result;
}

}  // namespace chatterino

#endif

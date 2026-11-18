// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/Restart.hpp"

#include "common/QLogging.hpp"

#include <QApplication>
#include <QProcess>
#include <QSettings>
#include <QStringBuilder>

using namespace Qt::Literals;

namespace {

// Runs on the host with the app id, branch and instance id. Starts the
// Flatpak once this instance is gone, which is the new version after an
// update. It runs in the background so flatpak-spawn returns right away,
// which would otherwise keep this instance alive.
// Host commands don't get the graphical session's environment, so it's taken
// from systemd. Without it the new instance has no display and aborts.
const QString FLATPAK_RESTART_SCRIPT = uR"sh(
(
    vars=$(systemctl --user show-environment 2>/dev/null |
        grep -E '^(DISPLAY|WAYLAND_DISPLAY|XAUTHORITY|XDG_SESSION_TYPE|XDG_CURRENT_DESKTOP)=')
    set -f
    IFS='
'
    for var in $vars; do
        export "$var"
    done
    unset IFS
    set +f

    i=0
    while [ "$i" -lt 30 ] && flatpak ps --columns=instance | grep -qxF "$3"; do
        sleep 1
        i=$((i + 1))
    done
    exec flatpak run --branch="$2" "$1"
) </dev/null >/dev/null 2>&1 &
)sh"_s;

bool restartPending = false;

/// The arguments this instance got, for the one replacing it
QStringList restartArguments()
{
    QStringList args;
    const auto current = QCoreApplication::arguments();
    for (qsizetype i = 1; i < current.size(); ++i)
    {
        const auto &arg = current.at(i);
        // only meant for the start after a crash
        if (arg == u"--crash-recovery"_s || arg == u"--remote-restart"_s ||
            arg.startsWith(u"--cr-exception-code="_s) ||
            arg.startsWith(u"--cr-exception-message="_s))
        {
            continue;
        }
        if (arg == u"--cr-exception-code"_s ||
            arg == u"--cr-exception-message"_s)
        {
            ++i;  // and its value
            continue;
        }
        args.append(arg);
    }
    // don't hand off to this instance if it's still quitting
    args.append(u"--remote-restart"_s);
    return args;
}

}  // namespace

namespace chatterino {

std::optional<FlatpakInstance> readFlatpakInstance()
{
    const QSettings info(u"/.flatpak-info"_s, QSettings::IniFormat);
    FlatpakInstance instance{
        .appId = info.value(u"Application/name"_s).toString(),
        .branch = info.value(u"Instance/branch"_s).toString(),
        .commit = info.value(u"Instance/app-commit"_s).toString(),
        .instanceId = info.value(u"Instance/instance-id"_s).toString(),
    };
    if (instance.appId.isEmpty() || instance.branch.isEmpty() ||
        instance.commit.isEmpty() || instance.instanceId.isEmpty())
    {
        return std::nullopt;
    }
    return instance;
}

bool startFlatpakRestart(const FlatpakInstance &instance)
{
    QStringList args{u"--host"_s};
    // for systems where systemd doesn't have the session's environment.
    // Flatpak keeps the Wayland socket's name, so these match the host.
    for (const auto *name : {"WAYLAND_DISPLAY", "XDG_SESSION_TYPE"})
    {
        const auto value = qEnvironmentVariable(name);
        if (!value.isEmpty())
        {
            args.append(u"--env="_s % QLatin1StringView(name) % u'=' % value);
        }
    }
    args.append({u"sh"_s, u"-c"_s, FLATPAK_RESTART_SCRIPT, u"sh"_s,
                 instance.appId, instance.branch, instance.instanceId});

    // only quit once the host has the restart, it's quick
    QProcess restart;
    restart.start(u"flatpak-spawn"_s, args);
    if (!restart.waitForFinished(10000) || restart.exitCode() != 0)
    {
        qCWarning(chatterinoApp)
            << "Failed to have the host restart the Flatpak"
            << restart.errorString();
        return false;
    }
    return true;
}

bool restartApplication()
{
    if (const auto instance = readFlatpakInstance())
    {
        if (!startFlatpakRestart(*instance))
        {
            return false;
        }
    }
    else
    {
        restartPending = true;
    }

    QApplication::exit(0);
    return true;
}

void startPendingRestart()
{
    if (!restartPending)
    {
        return;
    }

    // an AppImage's files go away when it quits
    auto program = qEnvironmentVariable("APPIMAGE");
    if (program.isEmpty())
    {
        program = QCoreApplication::applicationFilePath();
    }
    if (!QProcess::startDetached(program, restartArguments()))
    {
        qCWarning(chatterinoApp)
            << "Failed to start" << program << "after quitting";
    }
}

}  // namespace chatterino

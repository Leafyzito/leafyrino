// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/ProviderId.hpp"
#include "common/WindowDescriptors.hpp"

#include <QApplication>

#include <optional>

namespace chatterino {

class Paths;

class Args
{
public:
    struct Channel {
        ProviderId provider;
        QString name;
    };

    Args() = default;
    Args(const QApplication &app, const Paths &paths);

    bool printVersion{};

    bool crashRecovery{};
    bool remoteRestart{};

    std::optional<uint32_t> exceptionCode{};

    std::optional<QString> exceptionMessage{};

    bool shouldRunBrowserExtensionHost{};

    bool isFramelessEmbed{};
    std::optional<unsigned long long> parentWindowId{};

    bool dontSaveSettings{};
    bool dontLoadMainWindow{};
    std::optional<WindowLayout> customChannelLayout;
    std::optional<Channel> activateChannel;
    std::optional<QString> initialLogin;
    bool verbose{};
    bool safeMode{};
    bool useOldScaling{};

#ifndef NDEBUG

    bool useLocalEventsub = false;
#endif

    QStringList currentArguments() const;

private:
    void applyCustomChannelLayout(const QString &argValue, const Paths &paths);

    QStringList currentArguments_;
};

}

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
    explicit Args(const QApplication &app);

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
    std::vector<Channel> customChannels;
    std::optional<Channel> activateChannel;
    std::optional<QString> initialLogin;
    bool verbose{};
    bool safeMode{};

    bool portableEnable{};
    std::optional<QString> portableDirectory;

    bool useOldScaling = false;

#ifndef NDEBUG

    bool useLocalEventsub = false;
#endif

    QStringList currentArguments() const;
    std::optional<WindowLayout> makeCustomChannelLayout(
        const QString &windowLayoutFile) const;

private:
    QStringList currentArguments_;
};

}  // namespace chatterino

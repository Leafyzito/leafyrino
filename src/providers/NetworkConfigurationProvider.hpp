// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QNetworkProxy>
#include <twitch-eventsub-ws/proxy.hpp>

#include <optional>

class QString;

namespace chatterino {

class Env;

class NetworkConfigurationProvider
{
public:
    NetworkConfigurationProvider() = delete;

    static void applyFromEnv(const Env &env);

    /// Parses a proxy URL (like CHATTERINO2_PROXY_URL) for connections that
    /// don't go through Qt (i.e. WebSockets).
    static std::optional<eventsub::lib::ProxyConfig> parseWebSocketProxy(
        const QString &url);

    /// The proxy WebSockets should connect through, if any. Safe to call from
    /// any thread.
    static const std::optional<eventsub::lib::ProxyConfig> &webSocketProxy();
};

}  // namespace chatterino

// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/NetworkConfigurationProvider.hpp"

#include "common/Env.hpp"
#include "common/QLogging.hpp"

#include <QNetworkProxy>
#include <QSslConfiguration>
#include <QUrl>

namespace {

QNetworkProxy createProxyFromUrl(const QUrl &url)
{
    QNetworkProxy proxy;
    proxy.setHostName(url.host(QUrl::FullyEncoded));
    proxy.setUser(url.userName(QUrl::FullyEncoded));
    proxy.setPassword(url.password(QUrl::FullyEncoded));
    proxy.setPort(url.port(1080));

    if (url.scheme().compare(QStringLiteral("socks5"), Qt::CaseInsensitive) ==
        0)
    {
        proxy.setType(QNetworkProxy::Socks5Proxy);
    }
    else
    {
        proxy.setType(QNetworkProxy::HttpProxy);
        if (!proxy.user().isEmpty() || !proxy.password().isEmpty())
        {
            const auto auth = proxy.user() + ":" + proxy.password();
            const auto base64 = auth.toUtf8().toBase64();
            proxy.setRawHeader("Proxy-Authorization",
                               QByteArray("Basic ").append(base64));
        }
    }

    return proxy;
}

void applyProxy(const QString &url)
{
    auto proxyUrl = QUrl(url);
    if (!proxyUrl.isValid() || proxyUrl.isEmpty())
    {
        qCDebug(chatterinoNetwork)
            << "Invalid or empty proxy url: " << proxyUrl;
        return;
    }

    const auto proxy = createProxyFromUrl(proxyUrl);

    QNetworkProxy::setApplicationProxy(proxy);
    qCDebug(chatterinoNetwork) << "Set application proxy to" << proxy;
}

}  // namespace

namespace chatterino {

void NetworkConfigurationProvider::applyFromEnv(const Env &env)
{
    if (env.proxyUrl)
    {
        applyProxy(*env.proxyUrl);
    }
}

std::optional<eventsub::lib::ProxyConfig>
    NetworkConfigurationProvider::parseWebSocketProxy(const QString &url)
{
    auto proxyUrl = QUrl(url);
    if (!proxyUrl.isValid() || proxyUrl.isEmpty())
    {
        return std::nullopt;
    }

    // same rules as createProxyFromUrl
    eventsub::lib::ProxyConfig proxy;
    proxy.type = proxyUrl.scheme().compare(QStringLiteral("socks5"),
                                           Qt::CaseInsensitive) == 0
                     ? eventsub::lib::ProxyConfig::Type::Socks5
                     : eventsub::lib::ProxyConfig::Type::Http;
    proxy.host = proxyUrl.host(QUrl::FullyDecoded).toStdString();
    proxy.port = static_cast<uint16_t>(proxyUrl.port(1080));
    proxy.username = proxyUrl.userName(QUrl::FullyDecoded).toStdString();
    proxy.password = proxyUrl.password(QUrl::FullyDecoded).toStdString();

    if (proxy.host.empty())
    {
        return std::nullopt;
    }

    return proxy;
}

const std::optional<eventsub::lib::ProxyConfig> &
    NetworkConfigurationProvider::webSocketProxy()
{
    static const auto proxy = [] {
        const auto &env = Env::get();
        if (!env.proxyUrl)
        {
            return std::optional<eventsub::lib::ProxyConfig>{};
        }
        return parseWebSocketProxy(*env.proxyUrl);
    }();
    return proxy;
}

}  // namespace chatterino

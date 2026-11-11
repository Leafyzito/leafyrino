// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/NetworkConfigurationProvider.hpp"

#include "Test.hpp"

using namespace chatterino;
using Type = eventsub::lib::ProxyConfig::Type;

TEST(NetworkConfigurationProvider, parseWebSocketProxyHttp)
{
    auto proxy =
        NetworkConfigurationProvider::parseWebSocketProxy("http://proxy:8080");
    ASSERT_TRUE(proxy);
    EXPECT_EQ(proxy->type, Type::Http);
    EXPECT_EQ(proxy->host, "proxy");
    EXPECT_EQ(proxy->port, 8080);
    EXPECT_FALSE(proxy->hasCredentials());
}

TEST(NetworkConfigurationProvider, parseWebSocketProxySocks5)
{
    auto proxy = NetworkConfigurationProvider::parseWebSocketProxy(
        "SOCKS5://us%40r:p%3Ass@127.0.0.1");
    ASSERT_TRUE(proxy);
    EXPECT_EQ(proxy->type, Type::Socks5);
    EXPECT_EQ(proxy->host, "127.0.0.1");
    EXPECT_EQ(proxy->port, 1080);
    EXPECT_EQ(proxy->username, "us@r");
    EXPECT_EQ(proxy->password, "p:ss");
}

TEST(NetworkConfigurationProvider, parseWebSocketProxyIpv6)
{
    auto proxy = NetworkConfigurationProvider::parseWebSocketProxy(
        "socks5://[::1]:9050");
    ASSERT_TRUE(proxy);
    EXPECT_EQ(proxy->host, "::1");
    EXPECT_EQ(proxy->port, 9050);
}

TEST(NetworkConfigurationProvider, parseWebSocketProxyInvalid)
{
    EXPECT_FALSE(NetworkConfigurationProvider::parseWebSocketProxy(""));
    EXPECT_FALSE(NetworkConfigurationProvider::parseWebSocketProxy("http://"));
}

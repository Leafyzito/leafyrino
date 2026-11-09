#include "twitch-eventsub-ws/proxy.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <gtest/gtest.h>

#include <functional>
#include <thread>

using namespace chatterino::eventsub::lib;
using namespace std::chrono_literals;
namespace asio = boost::asio;
using asio::ip::tcp;
using boost::system::error_code;

namespace {

std::string readExactly(tcp::socket &sock, std::size_t n)
{
    std::string s(n, '\0');
    asio::read(sock, asio::buffer(s));
    return s;
}

std::string readHttpRequest(tcp::socket &sock)
{
    std::string s;
    asio::read_until(sock, asio::dynamic_buffer(s), "\r\n\r\n");
    return s;
}

void writeAll(tcp::socket &sock, std::string_view s)
{
    asio::write(sock, asio::buffer(s));
}

std::string bytes(std::initializer_list<int> b)
{
    std::string s;
    for (int x : b)
    {
        s.push_back(static_cast<char>(x));
    }
    return s;
}

/// pretend to be the target: echo what the client sends after the handshake
void echoOnce(tcp::socket &sock)
{
    writeAll(sock, readExactly(sock, 5));
}

struct Result {
    error_code ec;
    std::string echoed;
};

/// Connects to a fake proxy running `server` and does a handshake for
/// targetHost:targetPort. If it succeeds, "hello" is sent through the tunnel
/// and the response is returned.
Result runHandshake(ProxyConfig proxy, std::string targetHost,
                    std::uint16_t targetPort,
                    std::function<void(tcp::socket &)> server)
{
    asio::io_context serverIoc;
    tcp::acceptor acceptor(serverIoc, {asio::ip::make_address("127.0.0.1"), 0});
    auto proxyEndpoint = acceptor.local_endpoint();
    proxy.host = "127.0.0.1";
    proxy.port = proxyEndpoint.port();

    std::thread serverThread([&] {
        tcp::socket sock(serverIoc);
        acceptor.accept(sock);
        try
        {
            server(sock);
        }
        catch (const boost::system::system_error &)
        {
            // the client went away, the test checks the result
        }
    });

    asio::io_context ioc;
    boost::beast::tcp_stream stream(ioc);
    Result result;
    stream.expires_after(5s);
    stream.async_connect(proxyEndpoint, [&](error_code ec) {
        if (ec)
        {
            result.ec = ec;
            return;
        }
        asyncProxyHandshake(
            stream, proxy, targetHost, targetPort, [&](error_code ec) {
                result.ec = ec;
                if (ec)
                {
                    stream.close();
                    return;
                }
                asio::async_write(stream, asio::buffer("hello", 5),
                                  [&](error_code ec, std::size_t) {
                                      if (ec)
                                      {
                                          result.ec = ec;
                                          return;
                                      }
                                      result.echoed.resize(5);
                                      asio::async_read(
                                          stream, asio::buffer(result.echoed),
                                          [&](error_code ec, std::size_t) {
                                              if (ec)
                                              {
                                                  result.ec = ec;
                                              }
                                          });
                                  });
            });
    });
    ioc.run();
    serverThread.join();

    return result;
}

ProxyConfig httpProxy(std::string user = {}, std::string pass = {})
{
    return {
        .type = ProxyConfig::Type::Http,
        .username = std::move(user),
        .password = std::move(pass),
    };
}

ProxyConfig socksProxy(std::string user = {}, std::string pass = {})
{
    return {
        .type = ProxyConfig::Type::Socks5,
        .username = std::move(user),
        .password = std::move(pass),
    };
}

}  // namespace

TEST(Proxy, httpConnect)
{
    auto res = runHandshake(httpProxy(), "example.com", 443, [](auto &sock) {
        EXPECT_EQ(readHttpRequest(sock), "CONNECT example.com:443 HTTP/1.1\r\n"
                                         "Host: example.com:443\r\n\r\n");
        writeAll(sock, "HTTP/1.1 200 Connection established\r\n\r\n");
        echoOnce(sock);
    });
    EXPECT_FALSE(res.ec) << res.ec.message();
    EXPECT_EQ(res.echoed, "hello");
}

TEST(Proxy, httpConnectWithAuth)
{
    auto res = runHandshake(
        httpProxy("user", "p@ss:word"), "example.com", 80, [](auto &sock) {
            EXPECT_EQ(readHttpRequest(sock),
                      "CONNECT example.com:80 HTTP/1.1\r\n"
                      "Host: example.com:80\r\n"
                      "Proxy-Authorization: Basic dXNlcjpwQHNzOndvcmQ=\r\n"
                      "\r\n");
            writeAll(sock, "HTTP/1.0 200 OK\r\nVia: test\r\n\r\n");
            echoOnce(sock);
        });
    EXPECT_FALSE(res.ec) << res.ec.message();
    EXPECT_EQ(res.echoed, "hello");
}

TEST(Proxy, httpConnectIpv6Target)
{
    auto res = runHandshake(httpProxy(), "::1", 443, [](auto &sock) {
        EXPECT_EQ(readHttpRequest(sock), "CONNECT [::1]:443 HTTP/1.1\r\n"
                                         "Host: [::1]:443\r\n\r\n");
        writeAll(sock, "HTTP/1.1 200 OK\r\n\r\n");
        echoOnce(sock);
    });
    EXPECT_FALSE(res.ec) << res.ec.message();
    EXPECT_EQ(res.echoed, "hello");
}

TEST(Proxy, httpConnectRejected)
{
    auto res = runHandshake(httpProxy(), "example.com", 443, [](auto &sock) {
        readHttpRequest(sock);
        writeAll(sock, "HTTP/1.1 407 Proxy Authentication Required\r\n"
                       "Proxy-Authenticate: Basic\r\n\r\n");
    });
    EXPECT_EQ(res.ec, proxy::makeCode(proxy::Error::HttpStatus, 407));
    EXPECT_EQ(res.ec.message(), "HTTP proxy responded with status 407");
}

TEST(Proxy, httpConnectGarbage)
{
    auto res = runHandshake(httpProxy(), "example.com", 443, [](auto &sock) {
        readHttpRequest(sock);
        writeAll(sock, "SSH-2.0-OpenSSH\r\n\r\n");
    });
    EXPECT_EQ(res.ec, proxy::makeCode(proxy::Error::InvalidResponse));
}

TEST(Proxy, socks5Domain)
{
    auto res = runHandshake(socksProxy(), "example.com", 443, [](auto &sock) {
        EXPECT_EQ(readExactly(sock, 3), bytes({5, 1, 0}));
        writeAll(sock, bytes({5, 0}));
        // CONNECT example.com:443 with the name resolved by the proxy
        EXPECT_EQ(readExactly(sock, 5 + 11 + 2),
                  bytes({5, 1, 0, 3, 11}) + "example.com" + bytes({1, 0xbb}));
        writeAll(sock, bytes({5, 0, 0, 1, 10, 0, 0, 1, 0x12, 0x34}));
        echoOnce(sock);
    });
    EXPECT_FALSE(res.ec) << res.ec.message();
    EXPECT_EQ(res.echoed, "hello");
}

TEST(Proxy, socks5Ipv4AndDomainBoundAddress)
{
    auto res = runHandshake(socksProxy(), "10.1.2.3", 80, [](auto &sock) {
        EXPECT_EQ(readExactly(sock, 3), bytes({5, 1, 0}));
        writeAll(sock, bytes({5, 0}));
        EXPECT_EQ(readExactly(sock, 10),
                  bytes({5, 1, 0, 1, 10, 1, 2, 3, 0, 80}));
        // bound address as a domain name, has to be skipped entirely
        writeAll(sock, bytes({5, 0, 0, 3, 3}) + "foo" + bytes({0, 1}));
        echoOnce(sock);
    });
    EXPECT_FALSE(res.ec) << res.ec.message();
    EXPECT_EQ(res.echoed, "hello");
}

TEST(Proxy, socks5Ipv6)
{
    auto res = runHandshake(socksProxy(), "::1", 443, [](auto &sock) {
        EXPECT_EQ(readExactly(sock, 3), bytes({5, 1, 0}));
        writeAll(sock, bytes({5, 0}));
        EXPECT_EQ(readExactly(sock, 4 + 16 + 2),
                  bytes({5, 1, 0, 4, 0, 0, 0, 0, 0, 0, 0,
                         0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0xbb}));
        writeAll(sock, bytes({5, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0,
                              0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1}));
        echoOnce(sock);
    });
    EXPECT_FALSE(res.ec) << res.ec.message();
    EXPECT_EQ(res.echoed, "hello");
}

TEST(Proxy, socks5WithAuth)
{
    auto res = runHandshake(
        socksProxy("user", "pass"), "example.com", 443, [](auto &sock) {
            EXPECT_EQ(readExactly(sock, 4), bytes({5, 2, 0, 2}));
            writeAll(sock, bytes({5, 2}));
            EXPECT_EQ(readExactly(sock, 11),
                      bytes({1, 4}) + "user" + bytes({4}) + "pass");
            writeAll(sock, bytes({1, 0}));
            EXPECT_EQ(
                readExactly(sock, 18),
                bytes({5, 1, 0, 3, 11}) + "example.com" + bytes({1, 0xbb}));
            writeAll(sock, bytes({5, 0, 0, 1, 0, 0, 0, 0, 0, 0}));
            echoOnce(sock);
        });
    EXPECT_FALSE(res.ec) << res.ec.message();
    EXPECT_EQ(res.echoed, "hello");
}

TEST(Proxy, socks5AuthFailed)
{
    auto res = runHandshake(socksProxy("user", "wrong"), "example.com", 443,
                            [](auto &sock) {
                                readExactly(sock, 4);
                                writeAll(sock, bytes({5, 2}));
                                readExactly(sock, 12);
                                writeAll(sock, bytes({1, 1}));
                            });
    EXPECT_EQ(res.ec, proxy::makeCode(proxy::Error::AuthFailed));
}

TEST(Proxy, socks5NoAcceptableMethod)
{
    auto res = runHandshake(socksProxy(), "example.com", 443, [](auto &sock) {
        readExactly(sock, 3);
        writeAll(sock, bytes({5, 0xff}));
    });
    EXPECT_EQ(res.ec, proxy::makeCode(proxy::Error::NoAcceptableAuthMethod));
}

TEST(Proxy, socks5ConnectRefused)
{
    auto res = runHandshake(socksProxy(), "example.com", 443, [](auto &sock) {
        readExactly(sock, 3);
        writeAll(sock, bytes({5, 0}));
        readExactly(sock, 18);
        writeAll(sock, bytes({5, 5, 0, 1, 0, 0, 0, 0, 0, 0}));
    });
    EXPECT_EQ(res.ec, proxy::makeCode(proxy::Error::Socks5Reply, 5));
    EXPECT_EQ(res.ec.message(), "SOCKS5: connection refused");
}

TEST(Proxy, proxyClosesEarly)
{
    auto res = runHandshake(socksProxy(), "example.com", 443, [](auto &sock) {
        readExactly(sock, 3);
    });
    EXPECT_EQ(res.ec, asio::error::eof);
}

// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/websockets/detail/BalancedResolverResults.hpp"
#include "common/websockets/detail/WebSocketConnection.hpp"

#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/websocket/stream.hpp>

namespace chatterino::ws::detail {

template <typename Derived, typename Inner>
class WebSocketConnectionHelper : public WebSocketConnection,
                                  public std::enable_shared_from_this<
                                      WebSocketConnectionHelper<Derived, Inner>>
{
public:
    using Stream = boost::beast::websocket::stream<Inner>;

    void post(auto &&fn);

    void run() final;
    void close() final;

    void sendText(const QByteArray &data) final;
    void sendBinary(const QByteArray &data) final;

protected:
    Derived *derived();

    void fail(boost::system::error_code ec, QStringView op);
    void fail(std::string_view ec, QStringView op);
    void doWsHandshake();

    void closeImpl();
    void trySend();

    Stream stream;

private:

    WebSocketConnectionHelper(WebSocketOptions options, int id,
                              std::unique_ptr<WebSocketListener> listener,
                              WebSocketPoolImpl *pool,
                              boost::asio::io_context &ioc, Stream stream);

    void onResolve(boost::system::error_code ec,
                   const boost::asio::ip::tcp::resolver::results_type &results);

    void tryConnect(std::optional<BalancedResolverResults::Entry> entry);
    void onTcpHandshake(const BalancedResolverResults::Entry &entry,
                        boost::system::error_code ec);
    void onWsHandshake(boost::system::error_code ec);

    void onReadDone(boost::system::error_code ec, size_t bytesRead);
    void onWriteDone(boost::system::error_code ec, size_t bytesWritten);

    friend Derived;

    BalancedResolverResults resolvedEndpoints;
};

class TlsWebSocketConnection
    : public WebSocketConnectionHelper<
          TlsWebSocketConnection,
          boost::asio::ssl::stream<boost::beast::tcp_stream>>
{
public:
    static constexpr int DEFAULT_PORT = 443;

    TlsWebSocketConnection(WebSocketOptions options, int id,
                           std::unique_ptr<WebSocketListener> listener,
                           WebSocketPoolImpl *pool,
                           boost::asio::io_context &ioc,
                           boost::asio::ssl::context &ssl);

protected:
    bool setupStream(const std::string &host);
    void afterTcpHandshake();

    friend WebSocketConnectionHelper<
        TlsWebSocketConnection,
        boost::asio::ssl::stream<boost::beast::tcp_stream>>;
};

class TcpWebSocketConnection
    : public WebSocketConnectionHelper<TcpWebSocketConnection,
                                       boost::beast::tcp_stream>
{
public:
    static constexpr int DEFAULT_PORT = 80;

    TcpWebSocketConnection(WebSocketOptions options, int id,
                           std::unique_ptr<WebSocketListener> listener,
                           WebSocketPoolImpl *pool,
                           boost::asio::io_context &ioc);

protected:
    void afterTcpHandshake();

    friend WebSocketConnectionHelper<TcpWebSocketConnection,
                                     boost::beast::tcp_stream>;
};

}

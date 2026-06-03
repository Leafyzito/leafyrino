// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/websockets/WebSocketPool.hpp"
#include "util/QByteArrayBuffer.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <QDebug>

#include <deque>
#include <memory>
#include <utility>

namespace chatterino::ws::detail {

class WebSocketPoolImpl;

class WebSocketConnection
{
public:
    WebSocketConnection(WebSocketOptions options, int id,
                        std::unique_ptr<WebSocketListener> listener,
                        WebSocketPoolImpl *pool, boost::asio::io_context &ioc);
    virtual ~WebSocketConnection();

    WebSocketConnection(const WebSocketConnection &) = delete;
    WebSocketConnection(WebSocketConnection &&) = delete;
    WebSocketConnection &operator=(const WebSocketConnection &) = delete;
    WebSocketConnection &operator=(WebSocketConnection &&) = delete;

    virtual void run() = 0;

    virtual void close() = 0;

    virtual void sendText(const QByteArray &data) = 0;

    virtual void sendBinary(const QByteArray &data) = 0;

protected:

    void detach();

    WebSocketOptions options;

    std::unique_ptr<WebSocketListener> listener;

    WebSocketPoolImpl *pool;

    boost::asio::ip::tcp::resolver resolver;

    std::deque<std::pair<bool, QByteArrayBuffer>> queuedMessages;
    bool isSending = false;
    bool isClosing = false;
    int id = 0;

    boost::beast::flat_buffer readBuffer;

    friend QDebug operator<<(QDebug dbg, const WebSocketConnection &conn);
};

}

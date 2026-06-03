// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/OnceFlag.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <QString>

#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

namespace chatterino::ws::detail {

class WebSocketConnection;

class WebSocketPoolImpl
{
public:
    WebSocketPoolImpl(const QString &shortName);
    ~WebSocketPoolImpl();

    WebSocketPoolImpl(const WebSocketPoolImpl &) = delete;
    WebSocketPoolImpl(WebSocketPoolImpl &&) = delete;
    WebSocketPoolImpl &operator=(const WebSocketPoolImpl &) = delete;
    WebSocketPoolImpl &operator=(WebSocketPoolImpl &&) = delete;

    void removeConnection(WebSocketConnection *conn);

    bool tryShutdown(std::chrono::milliseconds timeout);

    std::unique_ptr<std::thread> ioThread;
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type>
        work;

    std::vector<std::shared_ptr<WebSocketConnection>> connections;
    std::mutex connectionMutex;

    bool closing = false;
    int nextID = 1;

    OnceFlag shutdownFlag;
};

}  // namespace chatterino::ws::detail

// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/ExponentialBackoff.hpp"

#include <IrcConnection>
#include <pajlada/signals/signal.hpp>
#include <QTimer>

#include <chrono>

namespace chatterino {

class IrcConnection : public Communi::IrcConnection
{
public:
    IrcConnection(QObject *parent = nullptr);
    ~IrcConnection() override;

    pajlada::Signals::Signal<bool> connectionLost;

    pajlada::Signals::NoArgSignal heartbeat;

    void smartReconnect();

    virtual void open();
    virtual void close();

private:
    QTimer pingTimer_;
    QTimer reconnectTimer_;
    std::atomic<bool> recentlyReceivedMessage_{true};
    std::chrono::time_point<std::chrono::system_clock> lastPing_;

    ExponentialBackoff<5> reconnectBackoff_{std::chrono::milliseconds{1000}};

    std::atomic<bool> expectConnectionLoss_{false};

    std::atomic<bool> waitingForPong_{false};
};

}

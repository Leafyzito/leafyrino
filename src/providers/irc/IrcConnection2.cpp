// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/irc/IrcConnection2.hpp"

#include "common/QLogging.hpp"
#include "common/Version.hpp"

#include <chrono>

using namespace std::chrono_literals;

namespace chatterino {

namespace {

const auto payload = "chatterino/" + CHATTERINO_VERSION;

}

IrcConnection::IrcConnection(QObject *parent)
    : Communi::IrcConnection(parent)
{

    QObject::connect(this, &Communi::IrcConnection::socketError, this,
                     [](QAbstractSocket::SocketError error) {
                         qCDebug(chatterinoIrc) << "Connection error:" << error;
                     });

    QObject::connect(
        this, &Communi::IrcConnection::socketStateChanged, this,
        [this](QAbstractSocket::SocketState state) {
            if (state == QAbstractSocket::UnconnectedState)
            {
                this->pingTimer_.stop();

                if (!this->expectConnectionLoss_.load())
                {
                    this->connectionLost.invoke(false);
                }
            }
        });

    this->reconnectTimer_.setSingleShot(true);
    QObject::connect(&this->reconnectTimer_, &QTimer::timeout, [this] {
        if (this->isConnected())
        {

            qCDebug(chatterinoIrc) << "Reconnect: already reconnected";
        }
        else
        {
            qCDebug(chatterinoIrc) << "Reconnecting";
            this->open();
        }
    });

    this->pingTimer_.setInterval(5000);
    this->pingTimer_.start();
    this->lastPing_ = std::chrono::system_clock::now();
    QObject::connect(&this->pingTimer_, &QTimer::timeout, [this] {
        if (this->isConnected())
        {
            if (this->recentlyReceivedMessage_.load())
            {

                this->recentlyReceivedMessage_ = false;
                this->waitingForPong_ = false;

                auto now = std::chrono::system_clock::now();
                auto elapsed = now - this->lastPing_;
                if (elapsed < 3 * 5000ms)
                {
                    this->heartbeat.invoke();
                }
                else
                {
                    qCDebug(chatterinoIrc).nospace()
                        << "Got late ping (skipping heartbeat): "
                        << std::chrono::duration_cast<
                               std::chrono::milliseconds>(elapsed)
                               .count()
                        << "ms";
                }
                this->lastPing_ = now;

                return;
            }

            if (this->waitingForPong_.load())
            {

                this->close();
                this->connectionLost.invoke(true);
            }
            else
            {
                this->sendRaw("PING " + payload);
                this->waitingForPong_ = true;
            }
        }
    });

    QObject::connect(this, &Communi::IrcConnection::connected, this, [this] {
        this->pingTimer_.start();
    });

    QObject::connect(this, &Communi::IrcConnection::pongMessageReceived,
                     [this](Communi::IrcPongMessage *message) {
                         if (message->argument() == payload)
                         {
                             this->waitingForPong_ = false;
                         }
                     });

    QObject::connect(this, &Communi::IrcConnection::messageReceived,
                     [this](Communi::IrcMessage *message) {
                         this->recentlyReceivedMessage_ = true;

                         if (message->command() == "372")
                         {
                             this->reconnectBackoff_.reset();
                         }
                     });
}

IrcConnection::~IrcConnection()
{

    this->disconnect();
}

void IrcConnection::smartReconnect()
{
    if (this->reconnectTimer_.isActive())
    {

        return;
    }

    auto delay = this->reconnectBackoff_.next();
    qCDebug(chatterinoIrc) << "Reconnecting in" << delay.count() << "ms";
    this->reconnectTimer_.start(delay);
}

void IrcConnection::open()
{
    this->expectConnectionLoss_ = false;
    this->waitingForPong_ = false;
    this->recentlyReceivedMessage_ = false;
    Communi::IrcConnection::open();
}

void IrcConnection::close()
{
    this->expectConnectionLoss_ = true;
    Communi::IrcConnection::close();
}

}

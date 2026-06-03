// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/QLogging.hpp"
#include "common/websockets/WebSocketPool.hpp"
#include "debug/AssertInGuiThread.hpp"
#include "util/DebugCount.hpp"

#include <unordered_set>

namespace chatterino {

template <typename SubscriptionT, typename Derived>
class BasicPubSubClient
{
public:
    using Subscription = SubscriptionT;

    const size_t maxSubscriptions;

    BasicPubSubClient(size_t maxSubscriptions = 100)
        : maxSubscriptions(maxSubscriptions)
    {
    }

    ~BasicPubSubClient() = default;
    BasicPubSubClient(const BasicPubSubClient &) = delete;
    BasicPubSubClient(const BasicPubSubClient &&) = delete;
    BasicPubSubClient &operator=(const BasicPubSubClient &) = delete;
    BasicPubSubClient &operator=(const BasicPubSubClient &&) = delete;

    void onOpen()
    {
        assertInGuiThread();
        this->open_ = true;
    }

    void onMessage(const QByteArray & )
    {
    }

    void close()
    {
        this->ws_.close();
    }

    void sendText(const QByteArray &data)
    {
        this->ws_.sendText(data);
    }

    QByteArray encodeSubscription(const Subscription &subscription)
    {
        return subscription.encodeSubscribe();
    }

    QByteArray encodeUnsubscription(const Subscription &subscription)
    {
        return subscription.encodeUnsubscribe();
    }

protected:
    bool isSubscribed(const Subscription &subscription) const
    {
        return this->subscriptions_.contains(subscription);
    }

    bool subscribe(const Subscription &subscription)
    {
        if (this->subscriptions_.size() >= this->maxSubscriptions)
        {
            return false;
        }

        if (!this->subscriptions_.emplace(subscription).second)
        {
            qCWarning(chatterinoLiveupdates)
                << "Tried subscribing to" << subscription
                << "but we're already subscribed!";
            return true;
        }

        qCDebug(chatterinoLiveupdates) << "Subscribing to" << subscription;
        DebugCount::increase(DebugObject::LiveUpdatesSubscription);

        QByteArray encoded =
            static_cast<Derived *>(this)->encodeSubscription(subscription);
        this->ws_.sendText(encoded);

        return true;
    }

    bool unsubscribe(const Subscription &subscription)
    {
        if (this->subscriptions_.erase(subscription) <= 0)
        {
            return false;
        }

        qCDebug(chatterinoLiveupdates) << "Unsubscribing from" << subscription;
        DebugCount::decrease(DebugObject::LiveUpdatesSubscription);

        QByteArray encoded =
            static_cast<Derived *>(this)->encodeUnsubscription(subscription);
        this->ws_.sendText(encoded);

        return true;
    }

    bool isOpen() const
    {
        return this->open_;
    }

private:
    WebSocketHandle ws_;
    std::unordered_set<Subscription> subscriptions_;

    bool open_ = false;

    template <typename ManagerSubscription, typename ManagerClient>
    friend class BasicPubSubManager;
};

}

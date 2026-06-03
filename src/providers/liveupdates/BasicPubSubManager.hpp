// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/QLogging.hpp"
#include "common/websockets/WebSocketPool.hpp"
#include "providers/liveupdates/BasicPubSubListener.hpp"
#include "providers/liveupdates/Diag.hpp"
#include "util/DebugCount.hpp"
#include "util/ExponentialBackoff.hpp"

#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chatterino {

namespace liveupdates {

template <typename Manager, typename Client>
concept IsManager = requires(Manager &manager) {
    { manager.makeClient() } -> std::same_as<std::shared_ptr<Client>>;
};

template <typename Client>
concept IsClient = requires(Client &client, const QByteArray &msg) {
    { client.onOpen() } -> std::same_as<void>;
    { client.onMessage(msg) } -> std::same_as<void>;
    { client.close() } -> std::same_as<void>;
};

}

template <typename Derived, typename ClientT>
class BasicPubSubManager : public QObject
{
public:
    using Subscription = ClientT::Subscription;
    using Client = ClientT;

    BasicPubSubManager(QString host, QString shortName)
        : pool_(std::make_optional<WebSocketPool>(shortName))
        , host_(std::move(host))
    {

        static_assert(liveupdates::IsManager<Derived, Client>);
        static_assert(liveupdates::IsClient<Client>);
    }

    ~BasicPubSubManager() override
    {

        assert(this->stopping_);
    }

    BasicPubSubManager(const BasicPubSubManager &) = delete;
    BasicPubSubManager(const BasicPubSubManager &&) = delete;
    BasicPubSubManager &operator=(const BasicPubSubManager &) = delete;
    BasicPubSubManager &operator=(const BasicPubSubManager &&) = delete;

    liveupdates::Diag diag;

    void stop()
    {
        if (this->stopping_)
        {
            return;
        }

        this->stopping_ = true;
        this->pool_.reset();
    }

protected:
    void unsubscribe(const Subscription &subscription)
    {
        assertInGuiThread();

        for (auto &client : this->clients_)
        {
            if (client.second->unsubscribe(subscription))
            {
                return;
            }
        }
    }

    void subscribe(const Subscription &subscription)
    {
        assertInGuiThread();

        if (this->trySubscribe(subscription))
        {
            return;
        }

        this->addClient();
        this->pendingSubscriptions_.emplace_back(subscription);
        DebugCount::increase(DebugObject::LiveUpdatesSubscriptionBacklog);
    }

    const std::unordered_map<size_t, std::shared_ptr<Client>> &clients() const
    {
        return this->clients_;
    }

private:
    Derived *derived()
    {
        return static_cast<Derived *>(this);
    }

    void onConnectionOpen(size_t id)
    {
        assertInGuiThread();

        auto *client = this->resolve(id);
        if (client == nullptr)
        {
            qCWarning(chatterinoLiveupdates)
                << "Ignoring open event for unknown client:" << id;
            return;
        }

        DebugCount::increase(DebugObject::LiveUpdatesConnection);
        this->addingClient_ = false;
        this->diag.connectionsOpened.fetch_add(1, std::memory_order_acq_rel);

        this->connectBackoff_.reset();

        client->onOpen();
        auto pendingSubsToTake = std::min(this->pendingSubscriptions_.size(),
                                          client->maxSubscriptions);

        qCDebug(chatterinoLiveupdates)
            << "LiveUpdate connection opened, subscribing to"
            << pendingSubsToTake << "subscriptions!";

        while (pendingSubsToTake > 0 && !this->pendingSubscriptions_.empty())
        {
            const auto last = std::move(this->pendingSubscriptions_.back());
            this->pendingSubscriptions_.pop_back();
            if (this->isSubscribed(last))
            {

                continue;
            }

            if (!client->subscribe(last))
            {
                qCDebug(chatterinoLiveupdates)
                    << "Failed to subscribe to" << last << "on new client.";
                this->pendingSubscriptions_.emplace_back(std::move(last));
                break;
            }
            DebugCount::decrease(DebugObject::LiveUpdatesSubscriptionBacklog);
            pendingSubsToTake--;
        }

        if (!this->pendingSubscriptions_.empty())
        {
            qCDebug(chatterinoLiveupdates)
                << "Adding another client for "
                << this->pendingSubscriptions_.size() << "subs";
            this->addClient();
        }
    }

    void onConnectionClose(size_t id)
    {
        assertInGuiThread();

        auto it = this->clients_.find(id);
        if (it == this->clients_.end())
        {
            qCWarning(chatterinoLiveupdates) << "Unknown client:" << id;
            return;
        }

        this->addingClient_ = false;

        DebugCount::decrease(DebugObject::LiveUpdatesConnection);
        qCDebug(chatterinoLiveupdates) << "Connection" << id << "closed";

        auto subs = std::move(it->second->subscriptions_);
        bool wasOpen = it->second->isOpen();

        if (wasOpen)
        {
            this->diag.connectionsClosed.fetch_add(1,
                                                   std::memory_order::relaxed);
        }
        else
        {
            this->diag.connectionsFailed.fetch_add(1,
                                                   std::memory_order::relaxed);
        }

        this->clients_.erase(it);
        if (this->stopping_)
        {
            return;
        }

        if (!wasOpen)
        {
            qCWarning(chatterinoLiveupdates)
                << "Retrying after" << id << "failed";
            auto nSubs = subs.size();
            DebugCount::increase(DebugObject::LiveUpdatesSubscriptionBacklog,
                                 static_cast<int64_t>(nSubs));
            this->pendingSubscriptions_.insert(
                this->pendingSubscriptions_.end(),
                std::make_move_iterator(subs.begin()),
                std::make_move_iterator(subs.end()));

            QTimer::singleShot(this->connectBackoff_.next(), this, [this] {
                this->addClient();
            });
            return;
        }

        for (const auto &sub : subs)
        {
            this->subscribe(sub);
        }
    }

    void addClient()
    {
        assertInGuiThread();

        if (this->addingClient_ || !this->pool_)
        {
            return;
        }

        qCDebug(chatterinoLiveupdates) << "Adding an additional client";

        this->addingClient_ = true;

        auto id = this->nextId_++;
        auto client = this->derived()->makeClient();
        auto hdl = this->pool_->createSocket(
            WebSocketOptions{
                .url = this->host_,
                .headers = {},
            },
            std::make_unique<BasicPubSubListener<Derived>>(
                std::weak_ptr{client}, this->derived(), id));
        client->ws_ = std::move(hdl);
        this->clients_.emplace(id, std::move(client));
    }

    bool trySubscribe(const Subscription &subscription)
    {
        for (auto &client : this->clients_)
        {
            if (client.second->subscribe(subscription))
            {
                return true;
            }
        }
        return false;
    }

    bool isSubscribed(const Subscription &subscription) const
    {
        return std::ranges::any_of(this->clients_, [&](const auto &c) {
            return c.second->isSubscribed(subscription);
        });
    }

    Client *resolve(size_t id)
    {
        auto it = this->clients_.find(id);
        if (it == this->clients_.end())
        {
            return nullptr;
        }
        return it->second.get();
    }

    std::vector<Subscription> pendingSubscriptions_;
    ExponentialBackoff<5> connectBackoff_{std::chrono::milliseconds(1000)};

    std::optional<WebSocketPool> pool_;
    std::unordered_map<size_t, std::shared_ptr<Client>> clients_;

    const QString host_;

    size_t nextId_ = 0;

    bool stopping_ = false;
    bool addingClient_ = false;

    friend BasicPubSubListener<Derived>;
};

}

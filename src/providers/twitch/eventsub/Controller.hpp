// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/twitch/eventsub/SubscriptionHandle.hpp"
#include "providers/twitch/eventsub/SubscriptionRequest.hpp"
#include "twitch-eventsub-ws/logger.hpp"
#include "twitch-eventsub-ws/session.hpp"
#include "util/ExponentialBackoff.hpp"
#include "util/OnceFlag.hpp"
#include "util/ThreadGuard.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/functional/hash.hpp>
#include <QJsonObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>

namespace chatterino::eventsub {

class IController
{
public:
    virtual ~IController() = default;

    virtual void removeRef(const SubscriptionRequest &request) = 0;

    virtual void setQuitting() = 0;

    [[nodiscard]] virtual SubscriptionHandle subscribe(
        const SubscriptionRequest &request) = 0;

    virtual void reconnectConnection(
        std::unique_ptr<lib::Listener> connection,
        const std::optional<std::string> &reconnectURL,
        const std::unordered_set<SubscriptionRequest> &subs) = 0;

    virtual void debug() = 0;
};

class Controller : public IController
{
public:
    Controller();
    ~Controller() override;

    void removeRef(const SubscriptionRequest &request) override;

    void setQuitting() override;

    [[nodiscard]] SubscriptionHandle subscribe(
        const SubscriptionRequest &request) override;

    void reconnectConnection(
        std::unique_ptr<lib::Listener> connection,
        const std::optional<std::string> &reconnectURL,
        const std::unordered_set<SubscriptionRequest> &subs) override;

    void debug() override;

private:
    void subscribe(const SubscriptionRequest &request, bool isRetry);

    void createConnection(bool alternateHelixAuth = false);
    void createConnection(std::string host, std::string port, std::string path,
                          std::unique_ptr<lib::Listener> listener);
    void registerConnection(std::weak_ptr<lib::Session> &&connection);

    void retrySubscription(const SubscriptionRequest &request);

    void markRequestSubscribed(const SubscriptionRequest &request,
                               std::weak_ptr<lib::Session> connection,
                               const QString &subscriptionID);

    void markRequestFailed(const SubscriptionRequest &request);

    void markRequestUnsubscribed(const SubscriptionRequest &request);

    void clearConnections();

    std::shared_ptr<lib::Logger> logProxy;

    const std::string userAgent;

    std::string eventSubHost;
    std::string eventSubPort;
    std::string eventSubPath;

    std::unique_ptr<std::thread> thread;
    std::unique_ptr<ThreadGuard> threadGuard;
    boost::asio::io_context ioContext;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type>
        work;

    std::vector<std::weak_ptr<lib::Session>> connections;

    [[nodiscard]] std::optional<std::shared_ptr<lib::Session>>
        getViableConnection(const SubscriptionRequest &request,
                            uint32_t &openButNotReadyConnections);

    struct Subscription {
        enum class State : uint8_t {

            Unsubscribed,

            Failed,

            Subscribing,

            Retrying,

            Subscribed,

            Unsubscribing,
        } state = State::Unsubscribed;

        int32_t refCount = 0;
        std::weak_ptr<lib::Session> connection;

        QString subscriptionID;

        std::unique_ptr<boost::asio::system_timer> retryTimer;

        ExponentialBackoff<6> backoff{std::chrono::milliseconds{500}};
    };

    std::mutex subscriptionsMutex;
    std::unordered_map<SubscriptionRequest, Subscription> subscriptions;

    std::atomic<bool> quitting = false;
    OnceFlag stoppedFlag;
};

class DummyController : public IController
{
public:
    ~DummyController() override = default;

    void removeRef(const SubscriptionRequest &request) override
    {
        (void)request;
    }

    void setQuitting() override
    {
    }

    [[nodiscard]] SubscriptionHandle subscribe(
        const SubscriptionRequest &request) override
    {
        (void)request;
        return {};
    }

    void reconnectConnection(
        std::unique_ptr<lib::Listener> connection,
        const std::optional<std::string> &reconnectURL,
        const std::unordered_set<SubscriptionRequest> &subs) override;

    void debug() override
    {
    }
};

}  // namespace chatterino::eventsub

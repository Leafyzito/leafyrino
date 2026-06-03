// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/QStringHash.hpp"

#include <QString>
#include <QTimer>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace chatterino {

class TwitchChannel;

class ITwitchLiveController
{
public:
    virtual ~ITwitchLiveController() = default;

    virtual void add(const std::shared_ptr<TwitchChannel> &newChannel) = 0;
};

class TwitchLiveController : public ITwitchLiveController
{
public:

    static constexpr std::chrono::seconds REFRESH_INTERVAL{30};

    static constexpr std::chrono::seconds IMMEDIATE_REQUEST_INTERVAL{1};

    static constexpr int BATCH_SIZE{100};

    TwitchLiveController();

    void add(const std::shared_ptr<TwitchChannel> &newChannel) override;

private:
    struct ChannelEntry {
        std::weak_ptr<TwitchChannel> ptr;
        bool wasChecked = false;
    };

    void request(std::optional<QStringList> optChannelIDs = std::nullopt);

    std::unordered_map<QString, ChannelEntry> channels;
    std::shared_mutex channelsMutex;

    std::unordered_set<QString> immediateRequests;
    std::mutex immediateRequestsMutex;

    QTimer refreshTimer;

    QTimer immediateRequestTimer;
};

}

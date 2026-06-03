// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/sound/ISoundController.hpp"
#include "util/OnceFlag.hpp"
#include "util/ThreadGuard.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <QByteArray>
#include <QString>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

struct ma_engine;
struct ma_device;
struct ma_resource_manager;
struct ma_context;
struct ma_sound;
struct ma_decoder;

namespace chatterino {

class MiniaudioBackend : public ISoundController
{
    enum class State : std::uint8_t {
        Uninitialized,
        Initialized,
        Failed,
        Stopping,
    };

    std::atomic<State> state{State::Uninitialized};

public:
    explicit MiniaudioBackend(bool keepEngineAlive_);
    ~MiniaudioBackend() override;

    void play(const QUrl &sound) final;

private:
    std::unique_ptr<ma_context> context;

    std::unique_ptr<ma_engine> engine;

    QByteArray defaultPingData;

    std::vector<std::unique_ptr<ma_decoder>> defaultPingDecoders;

    std::vector<std::unique_ptr<ma_sound>> defaultPingSounds;

    ThreadGuard tgPlay;

    std::chrono::system_clock::time_point lastSoundPlay;

    boost::asio::io_context ioContext{1};
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type>
        workGuard;
    std::unique_ptr<std::thread> audioThread;
    OnceFlag stoppedFlag;
    boost::asio::steady_timer sleepTimer;

    bool keepEngineAlive;

    friend class Application;
};

}  // namespace chatterino

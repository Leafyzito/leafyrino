// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>

namespace chatterino {

namespace detail {

/// Coalesces requests to give freed memory back to the OS.
///
/// A request runs the relief after `coalesceDelay`, and never sooner than
/// `minimumInterval` after the previous relief finished. Requests made while
/// one is scheduled are merged into it, requests made while the relief runs
/// schedule exactly one more.
class MemoryReliefCoordinator
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = std::chrono::milliseconds;
    using Task = std::function<void()>;

    using Now = std::function<TimePoint()>;
    using ScheduleTimer = std::function<void(Duration, Task)>;
    using Execute = std::function<void(Task)>;
    using Relief = std::function<void()>;

    MemoryReliefCoordinator(Duration coalesceDelay, Duration minimumInterval,
                            Now now, ScheduleTimer scheduleTimer,
                            Execute execute, Relief relief);

    void request();

private:
    void timerFired();
    void workerFinished();
    void scheduleAfter(Duration delay);

    const Duration coalesceDelay_;
    const Duration minimumInterval_;
    const Now now_;
    const ScheduleTimer scheduleTimer_;
    const Execute execute_;
    const Relief relief_;

    std::mutex mutex_;
    std::optional<TimePoint> lastCompleted_;
    bool scheduled_ = false;
    bool running_ = false;
    bool pending_ = false;
};

}  // namespace detail

/// Asks the allocator to return freed memory to the OS after a large amount
/// of memory was released.
///
/// The work is coalesced and runs on a worker thread. It's only done where the
/// allocator keeps freed pages around otherwise (glibc and macOS).
void requestMemoryPressureRelief();

}  // namespace chatterino

// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/MemoryReclaimer.hpp"

#include <QCoreApplication>
#include <QMetaObject>
#include <QThreadPool>
#include <QTimer>

#if defined(Q_OS_LINUX)
#    include <features.h>
#endif

#if defined(Q_OS_LINUX) && defined(__GLIBC__)
#    include <malloc.h>
#    define CHATTERINO_HAS_MEMORY_RELIEF
#elif defined(Q_OS_MACOS)
#    include <malloc/malloc.h>
#    define CHATTERINO_HAS_MEMORY_RELIEF
#endif

#include <algorithm>
#include <utility>

namespace chatterino {

namespace detail {

MemoryReliefCoordinator::MemoryReliefCoordinator(Duration coalesceDelay,
                                                 Duration minimumInterval,
                                                 Now now,
                                                 ScheduleTimer scheduleTimer,
                                                 Execute execute, Relief relief)
    : coalesceDelay_(coalesceDelay)
    , minimumInterval_(minimumInterval)
    , now_(std::move(now))
    , scheduleTimer_(std::move(scheduleTimer))
    , execute_(std::move(execute))
    , relief_(std::move(relief))
{
}

void MemoryReliefCoordinator::request()
{
    Duration delay{};
    {
        std::lock_guard lock(this->mutex_);
        if (this->running_)
        {
            this->pending_ = true;
            return;
        }
        if (this->scheduled_)
        {
            return;
        }

        const auto now = this->now_();
        auto runAt = now + this->coalesceDelay_;
        if (this->lastCompleted_)
        {
            runAt =
                std::max(runAt, *this->lastCompleted_ + this->minimumInterval_);
        }
        delay = std::chrono::duration_cast<Duration>(runAt - now);
        this->scheduled_ = true;
    }

    this->scheduleAfter(delay);
}

void MemoryReliefCoordinator::timerFired()
{
    {
        std::lock_guard lock(this->mutex_);
        if (!this->scheduled_)
        {
            return;
        }
        this->scheduled_ = false;
        if (this->running_)
        {
            this->pending_ = true;
            return;
        }
        this->running_ = true;
    }

    this->execute_([this] {
        this->relief_();
        this->workerFinished();
    });
}

void MemoryReliefCoordinator::workerFinished()
{
    bool schedulePending = false;
    {
        std::lock_guard lock(this->mutex_);
        if (!this->running_)
        {
            return;
        }
        this->running_ = false;
        this->lastCompleted_ = this->now_();
        if (this->pending_)
        {
            this->pending_ = false;
            this->scheduled_ = true;
            schedulePending = true;
        }
    }

    if (schedulePending)
    {
        this->scheduleAfter(this->minimumInterval_);
    }
}

void MemoryReliefCoordinator::scheduleAfter(Duration delay)
{
    this->scheduleTimer_(std::max(Duration::zero(), delay), [this] {
        this->timerFired();
    });
}

}  // namespace detail

#ifdef CHATTERINO_HAS_MEMORY_RELIEF

namespace {

// many frees tend to come in bursts, wait for the burst to end
constexpr auto COALESCE_DELAY = std::chrono::seconds{5};
// trimming walks the whole heap, don't do it too often
constexpr auto MINIMUM_INTERVAL = std::chrono::seconds{30};

void releaseFreeAllocatorPages()
{
#    if defined(Q_OS_LINUX)
    malloc_trim(0);
#    elif defined(Q_OS_MACOS)
    malloc_zone_pressure_relief(nullptr, 0);
#    endif
}

detail::MemoryReliefCoordinator &memoryReliefCoordinator()
{
    using Coordinator = detail::MemoryReliefCoordinator;

    // leaked on purpose, a relief might still run on a worker thread at exit
    static auto *coordinator = new Coordinator{
        std::chrono::duration_cast<Coordinator::Duration>(COALESCE_DELAY),
        std::chrono::duration_cast<Coordinator::Duration>(MINIMUM_INTERVAL),
        [] {
            return Coordinator::Clock::now();
        },
        [](Coordinator::Duration delay, Coordinator::Task task) {
            auto *app = QCoreApplication::instance();
            if (app == nullptr || QCoreApplication::closingDown())
            {
                return;
            }
            // this is also called from the worker thread, timers have to be
            // started on the thread of their context
            QMetaObject::invokeMethod(
                app,
                [delay, task = std::move(task)]() mutable {
                    QTimer::singleShot(delay, QCoreApplication::instance(),
                                       std::move(task));
                },
                Qt::QueuedConnection);
        },
        [](Coordinator::Task task) {
            QThreadPool::globalInstance()->start(std::move(task));
        },
        [] {
            releaseFreeAllocatorPages();
        },
    };
    return *coordinator;
}

}  // namespace

#endif

void requestMemoryPressureRelief()
{
#ifdef CHATTERINO_HAS_MEMORY_RELIEF
    if (QCoreApplication::instance() == nullptr ||
        QCoreApplication::closingDown())
    {
        return;
    }

    memoryReliefCoordinator().request();
#endif
}

}  // namespace chatterino

// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/MemoryReclaimer.hpp"

#include "Test.hpp"

#include <memory>
#include <vector>

using namespace chatterino;
using namespace std::chrono_literals;

namespace {

using Coordinator = detail::MemoryReliefCoordinator;

/// Fake clock, timers and worker thread for the coordinator
class FakeEnvironment
{
public:
    std::unique_ptr<Coordinator> makeCoordinator()
    {
        return std::make_unique<Coordinator>(
            5s, 30s,
            [this] {
                return this->now;
            },
            [this](Coordinator::Duration delay, Coordinator::Task task) {
                this->timers.push_back({delay, std::move(task)});
            },
            [this](Coordinator::Task task) {
                this->tasks.push_back(std::move(task));
            },
            [this] {
                ++this->reliefs;
            });
    }

    /// Moves the clock to the oldest timer and fires it
    void fireTimer()
    {
        ASSERT_FALSE(this->timers.empty());
        auto timer = std::move(this->timers.front());
        this->timers.erase(this->timers.begin());
        this->now += timer.delay;
        timer.task();
    }

    /// Runs the oldest task queued for the worker thread
    void runTask()
    {
        ASSERT_FALSE(this->tasks.empty());
        auto task = std::move(this->tasks.front());
        this->tasks.erase(this->tasks.begin());
        task();
    }

    struct Timer {
        Coordinator::Duration delay;
        Coordinator::Task task;
    };

    Coordinator::TimePoint now{};
    std::vector<Timer> timers;
    std::vector<Coordinator::Task> tasks;
    int reliefs = 0;
};

}  // namespace

TEST(MemoryReliefCoordinator, CoalescesRequests)
{
    FakeEnvironment env;
    auto coordinator = env.makeCoordinator();

    coordinator->request();
    coordinator->request();
    coordinator->request();

    ASSERT_EQ(env.timers.size(), 1);
    EXPECT_EQ(env.timers[0].delay.count(), 5'000);

    env.fireTimer();
    ASSERT_EQ(env.tasks.size(), 1);
    // the relief itself runs on the worker
    EXPECT_EQ(env.reliefs, 0);

    env.runTask();
    EXPECT_EQ(env.reliefs, 1);
    EXPECT_TRUE(env.timers.empty());
    EXPECT_TRUE(env.tasks.empty());
}

TEST(MemoryReliefCoordinator, WaitsForMinimumInterval)
{
    FakeEnvironment env;
    auto coordinator = env.makeCoordinator();

    coordinator->request();
    env.fireTimer();
    env.runTask();
    ASSERT_EQ(env.reliefs, 1);

    // the previous relief finished 10s ago, so the next one has to wait 20s
    env.now += 10s;
    coordinator->request();
    ASSERT_EQ(env.timers.size(), 1);
    EXPECT_EQ(env.timers[0].delay.count(), 20'000);

    env.fireTimer();
    env.runTask();
    EXPECT_EQ(env.reliefs, 2);

    // long after the minimum interval only the coalescing delay is left
    env.now += 1min;
    coordinator->request();
    ASSERT_EQ(env.timers.size(), 1);
    EXPECT_EQ(env.timers[0].delay.count(), 5'000);
}

TEST(MemoryReliefCoordinator, RequestsWhileRunningScheduleOneMore)
{
    FakeEnvironment env;
    auto coordinator = env.makeCoordinator();

    coordinator->request();
    env.fireTimer();
    ASSERT_EQ(env.tasks.size(), 1);

    // the relief is still running on the worker
    coordinator->request();
    coordinator->request();
    EXPECT_TRUE(env.timers.empty());

    env.runTask();
    EXPECT_EQ(env.reliefs, 1);
    ASSERT_EQ(env.timers.size(), 1);
    EXPECT_EQ(env.timers[0].delay.count(), 30'000);

    env.fireTimer();
    env.runTask();
    EXPECT_EQ(env.reliefs, 2);
    EXPECT_TRUE(env.timers.empty());
    EXPECT_TRUE(env.tasks.empty());
}

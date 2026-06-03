// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace chatterino {

class OnceFlag
{
public:
    OnceFlag();

    void set();

    bool waitFor(std::chrono::milliseconds ms);

    void wait();

    bool isSet();

private:
    std::mutex mutex;
    std::condition_variable condvar;
    std::atomic<bool> flag = false;
};

}

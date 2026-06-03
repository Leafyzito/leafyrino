// SPDX-FileCopyrightText: 2021 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>

namespace chatterino {

template <unsigned maxSteps>
class ExponentialBackoff
{
public:
    ExponentialBackoff(const std::chrono::milliseconds &start)
        : start_(start)
    {
        static_assert(maxSteps > 1, "maxSteps must be higher than 1");
    }

    [[nodiscard]] std::chrono::milliseconds next()
    {
        auto next = this->start_ * (1 << (this->step_ - 1));

        this->step_ += 1;

        if (this->step_ >= maxSteps)
        {
            this->step_ = maxSteps;
        }

        return next;
    }

    void reset()
    {
        this->step_ = 1;
    }

private:
    const std::chrono::milliseconds start_;
    unsigned step_ = 1;
};

}  // namespace chatterino

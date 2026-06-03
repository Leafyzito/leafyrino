// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/twitch/eventsub/SubscriptionRequest.hpp"

#include <memory>

namespace chatterino::eventsub {

struct RawSubscriptionHandle {
    const SubscriptionRequest request;

    RawSubscriptionHandle(SubscriptionRequest request_);

    ~RawSubscriptionHandle();
};

using SubscriptionHandle = std::unique_ptr<RawSubscriptionHandle>;

}  // namespace chatterino::eventsub

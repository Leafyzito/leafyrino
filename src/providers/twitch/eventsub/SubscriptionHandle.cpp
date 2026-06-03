// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/eventsub/SubscriptionHandle.hpp"

#include "Application.hpp"
#include "providers/twitch/eventsub/Controller.hpp"

namespace chatterino::eventsub {

RawSubscriptionHandle::RawSubscriptionHandle(SubscriptionRequest request_)
    : request(std::move(request_))
{
}

RawSubscriptionHandle::~RawSubscriptionHandle()
{
    auto *app = tryGetApp();
    if (app == nullptr)
    {
        return;
    }
    app->getEventSub()->removeRef(this->request);
}

}  // namespace chatterino::eventsub

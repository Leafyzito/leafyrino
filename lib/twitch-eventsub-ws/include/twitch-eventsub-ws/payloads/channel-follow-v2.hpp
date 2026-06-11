#pragma once

#include "twitch-eventsub-ws/payloads/subscription.hpp"
#include "twitch-eventsub-ws/string.hpp"

#include <boost/json.hpp>

#include <chrono>

namespace chatterino::eventsub::lib::payload::channel_follow::v2 {

struct Event {
    String userID;
    String userLogin;
    String userName;

    String broadcasterUserID;
    String broadcasterUserLogin;
    String broadcasterUserName;

    std::chrono::system_clock::time_point followedAt;
};

struct Payload {
    subscription::Subscription subscription;

    Event event;
};

#include "twitch-eventsub-ws/payloads/channel-follow-v2.inc"

}  // namespace chatterino::eventsub::lib::payload::channel_follow::v2

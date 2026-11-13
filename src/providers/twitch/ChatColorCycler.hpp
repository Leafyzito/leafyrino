// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <memory>

namespace chatterino {

class TwitchChannel;

/// Changes the user's chat color after each message they send, when
/// "Change my color after every message" is on.
class ChatColorCycler
{
public:
    ChatColorCycler();

    void onMessageSent(const std::shared_ptr<TwitchChannel> &channel);

private:
    struct State;
    // shared so pending color requests can tell if we're gone
    std::shared_ptr<State> state_;
};

}  // namespace chatterino

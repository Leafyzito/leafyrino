// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <functional>

namespace pajlada::Signals {
class SignalHolder;
}

namespace chatterino {

class PixmapButton;

void initUpdateButton(PixmapButton &button,
                      const std::function<void()> &relayout,
                      pajlada::Signals::SignalHolder &signalHolder);

}

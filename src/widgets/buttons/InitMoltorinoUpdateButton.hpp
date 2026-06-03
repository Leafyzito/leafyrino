#pragma once

#include <functional>

namespace pajlada::Signals {
class SignalHolder;
}

namespace chatterino {

class PixmapButton;

void initMoltorinoUpdateButton(PixmapButton &button,
                               const std::function<void()> &relayout,
                               pajlada::Signals::SignalHolder &signalHolder);

}  // namespace chatterino

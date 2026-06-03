// SPDX-FileCopyrightText: 2021 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

namespace chatterino {

enum class HotkeyCategory {
    PopupWindow,
    Split,
    SplitInput,
    Window,
};

struct HotkeyCategoryData {
    QString name;
    QString displayName;
};

}  // namespace chatterino

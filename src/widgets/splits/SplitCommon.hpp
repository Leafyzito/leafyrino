// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>

namespace chatterino {

/// Pixel width for the leading icon in split header strips (pinned message,
/// prediction, etc.). Shared so titles align when panels stack.
inline int splitHeaderIconColumnWidth(float scale)
{
    const float s = std::max(1.0f, scale);
    return static_cast<int>(std::clamp(18.0f * s, 16.0f, 24.0f));
}

enum class SplitDirection {
    Left,
    Above,
    Right,
    Below,
};

}  // namespace chatterino

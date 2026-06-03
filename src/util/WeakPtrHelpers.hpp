// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <memory>

namespace chatterino {

template <typename T>
bool weakOwnerEquals(const std::weak_ptr<T> &a,
                     const std::weak_ptr<T> &b) noexcept
{
    return !a.owner_before(b) && !b.owner_before(a);
}

}

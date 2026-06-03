// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

#include <vector>

namespace chatterino::completion {

template <typename T>
class Strategy
{
public:
    virtual ~Strategy() = default;

    virtual void apply(const std::vector<T> &items, std::vector<T> &output,
                       const QString &query) const = 0;
};

}

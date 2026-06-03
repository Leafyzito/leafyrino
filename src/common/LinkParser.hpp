// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

#include <optional>

namespace chatterino::linkparser {

struct Parsed {

    QStringView protocol;

    QStringView host;

    QStringView rest;

    QStringView link;

    bool hasPrefix(QStringView source) const noexcept
    {
        return this->link.begin() != source.begin();
    }

    QStringView prefix(QStringView source) const noexcept
    {
        return {source.data(), this->link.begin()};
    }

    bool hasSuffix(QStringView source) const noexcept
    {
        return this->link.end() != source.end();
    }

    QStringView suffix(QStringView source) const noexcept
    {
        return {
            this->link.begin() + this->link.size(),
            source.data() + source.length(),
        };
    }
};

std::optional<Parsed> parse(QStringView source) noexcept;

}

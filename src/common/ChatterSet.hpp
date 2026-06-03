// SPDX-FileCopyrightText: 2021 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/QStringHash.hpp"

#include <lrucache/lrucache.hpp>
#include <QString>

#include <unordered_set>
#include <vector>

namespace chatterino {

class ChatterSet
{
public:

    static constexpr size_t CHATTER_LIMIT = 2000;

    ChatterSet();

    void addRecentChatter(const QString &userName);

    void updateOnlineChatters(
        const std::unordered_set<QString> &lowerCaseUsernames);

    bool contains(const QString &userName) const;

    std::vector<QString> filterByPrefix(const QString &prefix) const;

    std::vector<std::pair<QString, QString>> all() const;

private:

    cache::lru_cache<QString, QString> items;
};

using ChatterSet = ChatterSet;

}

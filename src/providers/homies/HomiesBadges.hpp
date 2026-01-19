// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"
#include "util/QStringHash.hpp"

#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

class HomiesBadges
{
public:
    HomiesBadges();
    void loadHomiesBadges();

    std::optional<EmotePtr> getBadge(const UserId &id);
    std::optional<EmotePtr> getBadge2(const UserId &id);
    std::optional<EmotePtr> getBadge3(const UserId &id);

private:
    std::shared_mutex mutex_;

    std::unordered_map<QString, int> badgeMap;
    std::unordered_map<QString, int> badgeMap2;
    std::unordered_map<QString, int> badgeMap3;
    std::vector<EmotePtr> emotes;
    std::vector<EmotePtr> emotes2;
    std::vector<EmotePtr> emotes3;
};

}  // namespace chatterino

// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"

#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

class FolhinhaBadges
{
public:
    /**
     * Makes a network request to load FolhinhaBot user badges
     */
    FolhinhaBadges();

    /**
     * Returns the FolhinhaBot badge for the given user, if any
     */
    std::optional<EmotePtr> getBadge(const UserId &id);

private:
    void loadFolhinhaBadges();

    std::shared_mutex mutex_;

    /**
     * Maps Twitch user IDs to their badge emote
     * Guarded by mutex_
     */
    std::unordered_map<QString, EmotePtr> badgeMap;
};

}  // namespace chatterino

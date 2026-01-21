// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"

#include <QJsonObject>

#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

#if __has_include(<gtest/gtest_prod.h>)
#    include <gtest/gtest_prod.h>
#endif

#ifdef FRIEND_TEST
class FolhinhaBadges_PriorityResolution_Test;
#endif

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

    void loadFolhinhaBadges();

private:
    void applyBadgeJson(const QJsonObject &jsonRoot);

    std::shared_mutex mutex_;

    /**
     * Maps Twitch user IDs to their badge emote
     * Guarded by mutex_
     */
    std::unordered_map<QString, EmotePtr> badgeMap;

    /**
     * Maps Twitch user IDs to their badge priority
     * Guarded by mutex_
     */
    std::unordered_map<QString, int> badgePriority;

#ifdef FRIEND_TEST
    FRIEND_TEST(FolhinhaBadges, PriorityResolution);
    friend class ::FolhinhaBadges_PriorityResolution_Test;
#endif
};

}  // namespace chatterino

// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/homies/HomiesBadges.hpp"

#include "common/Aliases.hpp"
#include "common/Literals.hpp"
#include "Test.hpp"

#include <QString>

using namespace chatterino;
using namespace literals;

TEST(HomiesBadges, BasicInitialization)
{
    HomiesBadges badges;

    // Badges should be initialized (even if loading is async)
    // Initially, no badges should be available until API loads
    auto badge = badges.getBadge({u"123456"_s});
    // Badge may or may not be available depending on API response
    // This test just verifies the method doesn't crash
}

TEST(HomiesBadges, BadgeRetrieval)
{
    HomiesBadges badges;

    // Test retrieval for non-existent user
    auto badge = badges.getBadge({u"nonexistent"_s});
    EXPECT_FALSE(badge.has_value())
        << "Non-existent user should not have a badge";

    // Test retrieval for empty user ID
    auto emptyBadge = badges.getBadge({u""_s});
    EXPECT_FALSE(emptyBadge.has_value())
        << "Empty user ID should not have a badge";
}

TEST(HomiesBadges, Badge2Retrieval)
{
    HomiesBadges badges;

    // Test retrieval for non-existent user
    auto badge = badges.getBadge2({u"nonexistent"_s});
    EXPECT_FALSE(badge.has_value())
        << "Non-existent user should not have a badge2";

    // Test retrieval for empty user ID
    auto emptyBadge = badges.getBadge2({u""_s});
    EXPECT_FALSE(emptyBadge.has_value())
        << "Empty user ID should not have a badge2";
}

TEST(HomiesBadges, Badge3Retrieval)
{
    HomiesBadges badges;

    // Test retrieval for non-existent user
    auto badge = badges.getBadge3({u"nonexistent"_s});
    EXPECT_FALSE(badge.has_value())
        << "Non-existent user should not have a badge3";

    // Test retrieval for empty user ID
    auto emptyBadge = badges.getBadge3({u""_s});
    EXPECT_FALSE(emptyBadge.has_value())
        << "Empty user ID should not have a badge3";
}

// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/folhinha/FolhinhaBadges.hpp"

#include "common/Aliases.hpp"
#include "Test.hpp"

#include <QString>

using namespace chatterino;
using namespace literals;

TEST(FolhinhaBadges, BasicInitialization)
{
    FolhinhaBadges badges;

    // Badges should be initialized (even if loading is async)
    // Initially, no badges should be available until API loads
    auto badge = badges.getBadge({u"123456"_s});
    // Badge may or may not be available depending on API response
    // This test just verifies the method doesn't crash
}

TEST(FolhinhaBadges, BadgeRetrieval)
{
    FolhinhaBadges badges;

    // Test retrieval for non-existent user
    auto badge = badges.getBadge({u"nonexistent"_s});
    EXPECT_FALSE(badge.has_value())
        << "Non-existent user should not have a badge";

    // Test retrieval for empty user ID
    auto emptyBadge = badges.getBadge({u""_s});
    EXPECT_FALSE(emptyBadge.has_value())
        << "Empty user ID should not have a badge";
}

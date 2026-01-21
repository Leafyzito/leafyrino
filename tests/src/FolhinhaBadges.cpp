// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/folhinha/FolhinhaBadges.hpp"

#include "common/Aliases.hpp"
#include "common/Literals.hpp"
#include "messages/Emote.hpp"
#include "Test.hpp"

#include <QJsonArray>
#include <QJsonObject>
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

TEST(FolhinhaBadges, PriorityResolution)
{
    FolhinhaBadges badges;

    // Build a payload matching the FolhinhaBot API structure.
    QJsonObject root;

    // leafyzito is both dev and admin -> should take dev
    {
        QJsonArray dev;
        dev.push_back(QJsonObject{
            {"userid", "120209265"},
            {"currAlias", "leafyzito"},
        });
        root.insert("dev", dev);
    }
    {
        QJsonArray admins;
        admins.push_back(QJsonObject{
            {"userid", "120209265"},
            {"currAlias", "leafyzito"},
        });
        admins.push_back(QJsonObject{
            {"userid", "744028864"},
            {"currAlias", "onoffle"},
        });
        root.insert("admins", admins);
    }
    // onoffle is both admin and plus founder -> should take admin
    {
        QJsonArray plus;
        plus.push_back(QJsonObject{
            {"userid", "744028864"},
            {"currAlias", "onoffle"},
            {"isFounder", true},
        });
        root.insert("plus", plus);
    }

    // Private method - allowed via FRIEND_TEST in FolhinhaBadges.hpp
    badges.applyBadgeJson(root);

    {
        auto badge = badges.getBadge({u"120209265"_s});
        ASSERT_TRUE(badge.has_value());
        EXPECT_EQ((*badge)->tooltip.string, "FolhinhaBot Developer");
    }

    {
        auto badge = badges.getBadge({u"744028864"_s});
        ASSERT_TRUE(badge.has_value());
        EXPECT_EQ((*badge)->tooltip.string, "FolhinhaBot Admin");
    }
}

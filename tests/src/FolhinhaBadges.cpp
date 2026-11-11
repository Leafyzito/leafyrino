// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/folhinha/FolhinhaBadges.hpp"

#include "common/Aliases.hpp"
#include "common/Literals.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
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

    {
        QJsonObject badges;
        const auto tier = [](const char *base) {
            return QJsonObject{
                {"1", QString("%1/1.webp").arg(base)},
                {"2", QString("%1/2.webp").arg(base)},
                {"3", QString("%1/3.webp").arg(base)},
            };
        };
        badges.insert("dev", tier("https://folhinhabot.com/badges/dev"));
        badges.insert("admin", tier("https://folhinhabot.com/badges/admin"));
        badges.insert("founder",
                      tier("https://folhinhabot.com/badges/founder"));
        badges.insert("sub", tier("https://folhinhabot.com/badges/sub"));
        root.insert("badges", badges);
    }

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

TEST(FolhinhaBadges, TierResolution)
{
    FolhinhaBadges badges;

    QJsonObject root;

    {
        QJsonObject badgeAssets;
        const auto tier = [](const QString &kind) {
            return QJsonObject{
                {"1", u"https://folhinhabot.com/badges/%11.webp"_s.arg(kind)},
                {"2", u"https://folhinhabot.com/badges/%12.webp"_s.arg(kind)},
                {"3", u"https://folhinhabot.com/badges/%13.webp"_s.arg(kind)},
            };
        };
        for (const auto *kind : {"dev", "admin", "founder", "sub", "sub3m",
                                 "sub6m", "sub12m", "sub24m"})
        {
            badgeAssets.insert(kind, tier(kind));
        }
        root.insert("badges", badgeAssets);
    }

    const auto plusUser = [](const QString &userId, const QJsonValue &badge,
                             bool isFounder = false) {
        QJsonObject obj{
            {"userid", userId},
            {"currAlias", userId},
            {"isFounder", isFounder},
        };
        if (!badge.isUndefined())
        {
            obj.insert("badge", badge);
        }
        return obj;
    };

    QJsonArray plus{
        plusUser(u"1"_s, u"sub"_s),
        plusUser(u"3"_s, u"sub3m"_s),
        plusUser(u"6"_s, u"sub6m"_s),
        plusUser(u"12"_s, u"sub12m"_s),
        plusUser(u"24"_s, u"sub24m"_s),
        // Old API without per-user badge
        plusUser(u"old"_s, QJsonValue::Undefined),
        // Not in the badges map
        plusUser(u"unknown"_s, u"sub48m"_s),
        // Must not escalate to a staff badge
        plusUser(u"dev"_s, u"dev"_s),
        plusUser(u"null"_s, QJsonValue::Null),
        // Founders keep the founder badge whatever their tier
        plusUser(u"founder"_s, u"sub24m"_s, true),
    };
    root.insert("plus", plus);

    badges.applyBadgeJson(root);

    const auto expectBadge = [&](const QString &userId, const QString &tooltip,
                                 const QString &kind) {
        auto badge = badges.getBadge({userId});
        ASSERT_TRUE(badge.has_value()) << userId.toStdString();
        EXPECT_EQ((*badge)->tooltip.string, tooltip) << userId.toStdString();
        EXPECT_EQ((*badge)->images.getImage1()->url().string,
                  u"https://folhinhabot.com/badges/%11.webp"_s.arg(kind))
            << userId.toStdString();
        EXPECT_EQ((*badge)->images.getImage3()->url().string,
                  u"https://folhinhabot.com/badges/%13.webp"_s.arg(kind))
            << userId.toStdString();
    };

    expectBadge(u"1"_s, u"FolhinhaBot Plus"_s, u"sub"_s);
    expectBadge(u"3"_s, u"FolhinhaBot Plus · Folha (3+ meses)"_s, u"sub3m"_s);
    expectBadge(u"6"_s, u"FolhinhaBot Plus · Ramo (6+ meses)"_s, u"sub6m"_s);
    expectBadge(u"12"_s, u"FolhinhaBot Plus · Árvore (12+ meses)"_s,
                u"sub12m"_s);
    expectBadge(u"24"_s, u"FolhinhaBot Plus · Floresta (24+ meses)"_s,
                u"sub24m"_s);
    expectBadge(u"old"_s, u"FolhinhaBot Plus"_s, u"sub"_s);
    expectBadge(u"unknown"_s, u"FolhinhaBot Plus"_s, u"sub"_s);
    expectBadge(u"dev"_s, u"FolhinhaBot Plus"_s, u"sub"_s);
    expectBadge(u"null"_s, u"FolhinhaBot Plus"_s, u"sub"_s);
    expectBadge(u"founder"_s, u"FolhinhaBot Plus (Founder)"_s, u"founder"_s);
}

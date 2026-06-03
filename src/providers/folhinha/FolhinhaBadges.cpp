// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/folhinha/FolhinhaBadges.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/Outcome.hpp"
#include "common/QLogging.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QUrl>

#include <array>
#include <cstdint>

namespace chatterino {

namespace {

using namespace Qt::Literals;

enum class FolhinhaBadgePriority : std::uint8_t {
    Plus = 0,
    PlusFounder = 1,
    Admin = 2,
    Dev = 3,
};

struct BuiltBadge {
    EmotePtr emote;
    int priority = 0;
};

using BadgeImageUrls = std::array<QString, 3>;

std::unordered_map<QString, BadgeImageUrls> parseBadgeAssets(
    const QJsonObject &jsonRoot)
{
    std::unordered_map<QString, BadgeImageUrls> assets;

    const auto badgesObj = jsonRoot.value("badges").toObject();
    for (auto it = badgesObj.begin(); it != badgesObj.end(); ++it)
    {
        const auto tiers = it.value().toObject();
        BadgeImageUrls urls;
        for (size_t i = 0; i < urls.size(); ++i)
        {
            urls[i] = tiers.value(QString::number(i + 1)).toString();
        }
        if (!urls[0].isEmpty())
        {
            assets.emplace(it.key(), std::move(urls));
        }
    }

    return assets;
}

const BadgeImageUrls *lookupBadgeAssets(
    const std::unordered_map<QString, BadgeImageUrls> &assets,
    const QString &kind)
{
    const auto it = assets.find(kind);
    if (it == assets.end())
    {
        return nullptr;
    }
    return &it->second;
}

EmotePtr makeFolhinhaBadge(const QString &tooltip,
                           const BadgeImageUrls &urls)
{
    if (urls[0].isEmpty())
    {
        return nullptr;
    }

    auto emote = Emote{
        .name = EmoteName{u"folhinha:" % tooltip},
        .images =
            ImageSet{
                Image::fromUrl(Url{urls[0]}, 1.0, QSize(18, 18)),
                Image::fromUrl(Url{urls[1]}, 0.5, QSize(36, 36)),
                Image::fromUrl(Url{urls[2]}, 0.25, QSize(72, 72)),
            },
        .tooltip = Tooltip{tooltip},
        .homePage = Url{},
    };

    return std::make_shared<const Emote>(std::move(emote));
}

EmotePtr makeFolhinhaBadge(const QString &tooltip,
                           const std::unordered_map<QString, BadgeImageUrls> &assets,
                           const QString &kind)
{
    const auto *urls = lookupBadgeAssets(assets, kind);
    if (urls == nullptr)
    {
        return nullptr;
    }
    return makeFolhinhaBadge(tooltip, *urls);
}

void tryInsert(std::unordered_map<QString, BuiltBadge> &out,
               const QString &userId, BuiltBadge badge)
{
    if (userId.isEmpty() || !badge.emote)
    {
        return;
    }

    auto it = out.find(userId);
    if (it == out.end() || it->second.priority < badge.priority)
    {
        out[userId] = std::move(badge);
    }
}

}  // namespace

FolhinhaBadges::FolhinhaBadges()
{
    this->loadFolhinhaBadges();
}

std::optional<EmotePtr> FolhinhaBadges::getBadge(const UserId &id)
{
    std::shared_lock lock(this->mutex_);

    auto it = this->badgeMap.find(id.string);
    if (it != this->badgeMap.end())
    {
        return it->second;
    }
    return std::nullopt;
}

void FolhinhaBadges::loadFolhinhaBadges()
{
    static QUrl url("https://api.folhinhabot.com/plus");

    NetworkRequest(url)
        .concurrent()
        .onSuccess([this](auto result) -> Outcome {
            auto jsonRoot = result.parseJson();

            this->applyBadgeJson(jsonRoot);

            // Process supporters
            // TODO: Re-enable supporter badges when ready
            /*
            const auto supporterAssets = parseBadgeAssets(jsonRoot);
            if (jsonRoot.contains("supporters"))
            {
                auto supportersArray = jsonRoot.value("supporters").toArray();
                for (const auto &userValue : supportersArray)
                {
                    auto userObj = userValue.toObject();
                    auto userId = userObj.value("userid").toString();

                    if (userId.isEmpty())
                    {
                        continue;
                    }

                    // Check if user already has a plus badge (founder/supporter can overlap)
                    // Supporters badge takes precedence if user is both plus and supporter
                    // (this should be rare, but we'll handle it)
                    if (this->badgeMap.find(userId) != this->badgeMap.end())
                    {
                        // User already has a plus badge, skip supporter badge
                        continue;
                    }

                    const auto emote = makeFolhinhaBadge(
                        u"FolhinhaBot Supporter"_s, supporterAssets, u"sub"_s);
                    if (!emote)
                    {
                        continue;
                    }

                    this->badgeMap[userId] = emote;
                }
            }
            */

            return Success;
        })
        .execute();
}

void FolhinhaBadges::applyBadgeJson(const QJsonObject &jsonRoot)
{
    const auto badgeAssets = parseBadgeAssets(jsonRoot);

    std::unordered_map<QString, BuiltBadge> built;
    built.reserve(256);

    // Priority: dev > admin > plus founder > plus
    if (jsonRoot.contains("dev"))
    {
        const auto devArray = jsonRoot.value("dev").toArray();
        for (const auto &userValue : devArray)
        {
            const auto userObj = userValue.toObject();
            const auto userId = userObj.value("userid").toString();
            tryInsert(
                built, userId,
                BuiltBadge{
                    .emote = makeFolhinhaBadge(u"FolhinhaBot Developer"_s,
                                              badgeAssets, u"dev"_s),
                    .priority = static_cast<int>(FolhinhaBadgePriority::Dev),
                });
        }
    }

    if (jsonRoot.contains("admins"))
    {
        const auto adminsArray = jsonRoot.value("admins").toArray();
        for (const auto &userValue : adminsArray)
        {
            const auto userObj = userValue.toObject();
            const auto userId = userObj.value("userid").toString();
            tryInsert(
                built, userId,
                BuiltBadge{
                    .emote = makeFolhinhaBadge(u"FolhinhaBot Admin"_s,
                                              badgeAssets, u"admin"_s),
                    .priority = static_cast<int>(FolhinhaBadgePriority::Admin),
                });
        }
    }

    if (jsonRoot.contains("plus"))
    {
        const auto plusArray = jsonRoot.value("plus").toArray();
        for (const auto &userValue : plusArray)
        {
            const auto userObj = userValue.toObject();
            const auto userId = userObj.value("userid").toString();
            if (userId.isEmpty())
            {
                continue;
            }

            const bool isFounder = userObj.value("isFounder").toBool();
            if (isFounder)
            {
                tryInsert(
                    built, userId,
                    BuiltBadge{
                        .emote = makeFolhinhaBadge(
                            u"FolhinhaBot Plus (Founder)"_s, badgeAssets,
                            u"founder"_s),
                        .priority = static_cast<int>(
                            FolhinhaBadgePriority::PlusFounder),
                    });
            }
            else
            {
                tryInsert(built, userId,
                          BuiltBadge{
                              .emote = makeFolhinhaBadge(
                                  u"FolhinhaBot Plus"_s, badgeAssets, u"sub"_s),
                              .priority =
                                  static_cast<int>(FolhinhaBadgePriority::Plus),
                          });
            }
        }
    }

    if (built.empty() && !jsonRoot.contains("dev") &&
        !jsonRoot.contains("admins") && !jsonRoot.contains("plus"))
    {
        qCWarning(chatterinoNetwork) << "[FolhinhaBadges] Response missing "
                                        "expected fields from Folhinha "
                                        "API";
    }

    std::unique_lock lock(this->mutex_);
    this->badgeMap.clear();
    this->badgePriority.clear();
    this->badgeMap.reserve(built.size());
    this->badgePriority.reserve(built.size());

    for (auto &[userId, badge] : built)
    {
        this->badgeMap.emplace(userId, badge.emote);
        this->badgePriority.emplace(userId, badge.priority);
    }
}

}  // namespace chatterino

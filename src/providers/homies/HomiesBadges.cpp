// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "Application.hpp"
#include "providers/homies/HomiesBadges.hpp"

#include "common/Literals.hpp"
#include "common/QLogging.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "singletons/WindowManager.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringBuilder>
#include <QTimer>

#include <algorithm>
#include <chrono>

namespace chatterino {

namespace {

using namespace chatterino::literals;

using BadgeMap = std::unordered_map<QString, int>;
using BadgeStorage = std::vector<EmotePtr>;
constexpr auto HOMIES_USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36";
constexpr auto HOMIES_BADGE_FRAME_LIFETIME = std::chrono::minutes{4};

QJsonArray extractBadgesArray(const NetworkResult &result, const QString &sourceName)
{
    const auto object = result.parseJson();
    if (!object.isEmpty() && object.contains("badges") &&
        object.value("badges").isArray())
    {
        return object.value("badges").toArray();
    }

    const auto array = result.parseJsonArray();
    if (!array.isEmpty())
    {
        return array;
    }

    qCWarning(chatterinoApp) << "[Homies] Unexpected payload shape from"
                             << sourceName << "- expected { badges: [...] }"
                             << "or a root array.";
    return {};
}

EmotePtr createBadgeEmote(const QString &sourceTag, const QString &badgeName,
                          const QJsonObject &badgeJson)
{
    const auto image1 = badgeJson.value("image1").toString().trimmed();
    if (image1.isEmpty())
    {
        return nullptr;
    }

    const auto tooltip = badgeJson.value("tooltip").toString().trimmed();
    const auto resolvedName =
        !badgeName.trimmed().isEmpty() ? badgeName.trimmed()
                                       : (!tooltip.isEmpty() ? tooltip
                                                             : u"badge"_s);

    auto makeImage = [](const QString &url, qreal scale) {
        if (url.isEmpty())
        {
            return Image::getEmpty();
        }

        auto image = Image::fromUrl(Url{url}, scale);
        image->setFrameCacheLifetime(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                HOMIES_BADGE_FRAME_LIFETIME));
        return image;
    };

    auto emote = Emote{
        .name = EmoteName{sourceTag % u":" % resolvedName},
        .images = ImageSet{
            makeImage(image1, 1),
            makeImage(badgeJson.value("image2").toString().trimmed(), 0.5),
            makeImage(badgeJson.value("image3").toString().trimmed(), 0.25),
        },
        .tooltip = Tooltip{tooltip},
        .homePage = Url{},
        .id = EmoteId{image1},
    };

    return std::make_shared<const Emote>(std::move(emote));
}

void storeUserCentricBadges(const QJsonArray &badges, const QString &sourceTag,
                            BadgeMap &badgeMap, BadgeStorage &emotes)
{
    badgeMap.clear();
    emotes.clear();
    badgeMap.reserve(badges.size());
    emotes.reserve(badges.size());

    for (const auto &badgeValue : badges)
    {
        const auto badgeJson = badgeValue.toObject();
        const auto userId = badgeJson.value("userId").toString().trimmed();
        if (userId.isEmpty())
        {
            continue;
        }

        auto emote = createBadgeEmote(sourceTag, userId, badgeJson);
        if (!emote)
        {
            continue;
        }

        const int index = static_cast<int>(emotes.size());
        emotes.push_back(std::move(emote));
        badgeMap[userId] = index;
    }
}

void storeBadgeCentricBadges(const QJsonArray &badges, const QString &sourceTag,
                             BadgeMap &badgeMap, BadgeStorage &emotes)
{
    qsizetype userCountEstimate = 0;
    for (const auto &badgeValue : badges)
    {
        userCountEstimate += badgeValue.toObject().value("users").toArray().size();
    }

    badgeMap.clear();
    emotes.clear();
    badgeMap.reserve(static_cast<size_t>(std::max<qsizetype>(badges.size(), userCountEstimate)));
    emotes.reserve(badges.size());

    for (const auto &badgeValue : badges)
    {
        const auto badgeJson = badgeValue.toObject();
        auto emote = createBadgeEmote(sourceTag, badgeJson.value("tooltip").toString(),
                                      badgeJson);
        if (!emote)
        {
            continue;
        }

        const int index = static_cast<int>(emotes.size());
        emotes.push_back(std::move(emote));

        for (const auto &userValue : badgeJson.value("users").toArray())
        {
            const auto userId = userValue.toString().trimmed();
            if (!userId.isEmpty())
            {
                badgeMap[userId] = index;
            }
        }
    }
}

}

HomiesBadges::HomiesBadges()
{

    QTimer::singleShot(3500, [this] {
        this->startLoading();
    });
}

void HomiesBadges::startLoading()
{
    if (this->loadStarted_.exchange(true))
    {
        return;
    }

    this->loadHomiesBadges();
}

void HomiesBadges::queueRefresh()
{
    if (this->refreshQueued_.exchange(true))
    {
        return;
    }

    QTimer::singleShot(250, [this] {
        this->refreshQueued_.store(false);
        if (auto *windows = getApp()->getWindows())
        {
            windows->invalidateChannelViewBuffers();
        }
    });
}

void HomiesBadges::loadHomiesBadges()
{
    NetworkRequest("https://chatterinohomies.com/api/badges/list")
        .header("Accept", "application/json")
        .header("User-Agent", HOMIES_USER_AGENT)
        .timeout(5000)
        .concurrent()
        .onSuccess([this](const NetworkResult &result) {
            const auto badges = extractBadgesArray(
                result, u"chatterinohomies.com/api/badges/list"_s);

            BadgeMap badgeMap;
            BadgeStorage emotes;
            storeUserCentricBadges(badges, u"homies"_s, badgeMap, emotes);
            const auto loadedCount = emotes.size();

            {
                std::unique_lock lock(this->mutex_);
                this->badgeMap_ = std::move(badgeMap);
                this->emotes_ = std::move(emotes);
            }

            if (loadedCount > 0)
            {
                this->hasLoadedBadges_.store(true);
                this->queueRefresh();
            }

            qCDebug(chatterinoApp) << "[Homies] Loaded"
                                   << loadedCount
                                   << "badges from chatterinohomies.com";
        })
        .onError([](const NetworkResult &result) {
            qCWarning(chatterinoApp)
                << "[Homies] Failed to load chatterinohomies.com badges:"
                << result.formatError();
        })
        .execute();

    NetworkRequest("https://itzalex.github.io/badges")
        .header("Accept", "application/json")
        .header("User-Agent", HOMIES_USER_AGENT)
        .timeout(5000)
        .concurrent()
        .onSuccess([this](const NetworkResult &result) {
            const auto badges =
                extractBadgesArray(result, u"itzalex.github.io/badges"_s);

            BadgeMap badgeMap;
            BadgeStorage emotes;
            storeBadgeCentricBadges(badges, u"homies"_s, badgeMap, emotes);
            const auto loadedCount = emotes.size();

            {
                std::unique_lock lock(this->mutex_);
                this->badgeMap2_ = std::move(badgeMap);
                this->emotes2_ = std::move(emotes);
            }

            if (loadedCount > 0)
            {
                this->hasLoadedBadges_.store(true);
                this->queueRefresh();
            }

            qCDebug(chatterinoApp) << "[Homies] Loaded"
                                   << loadedCount
                                   << "badges from itzalex.github.io/badges";
        })
        .onError([](const NetworkResult &result) {
            qCWarning(chatterinoApp)
                << "[Homies] Failed to load itzalex.github.io/badges:"
                << result.formatError();
        })
        .execute();

    NetworkRequest("https://itzalex.github.io/badges2")
        .header("Accept", "application/json")
        .header("User-Agent", HOMIES_USER_AGENT)
        .timeout(5000)
        .concurrent()
        .onSuccess([this](const NetworkResult &result) {
            const auto badges =
                extractBadgesArray(result, u"itzalex.github.io/badges2"_s);

            BadgeMap badgeMap;
            BadgeStorage emotes;
            storeBadgeCentricBadges(badges, u"homies"_s, badgeMap, emotes);
            const auto loadedCount = emotes.size();

            {
                std::unique_lock lock(this->mutex_);
                this->badgeMap3_ = std::move(badgeMap);
                this->emotes3_ = std::move(emotes);
            }

            if (loadedCount > 0)
            {
                this->hasLoadedBadges_.store(true);
                this->queueRefresh();
            }

            qCDebug(chatterinoApp) << "[Homies] Loaded"
                                   << loadedCount
                                   << "badges from itzalex.github.io/badges2";
        })
        .onError([](const NetworkResult &result) {
            qCWarning(chatterinoApp)
                << "[Homies] Failed to load itzalex.github.io/badges2:"
                << result.formatError();
        })
        .execute();
}

std::array<EmotePtr, 3> HomiesBadges::getBadges(const QString &userId) const
{
    if (!this->hasLoadedBadges_.load())
    {
        return {};
    }

    std::shared_lock lock(this->mutex_);
    return {
        this->lookupBadge(this->badgeMap_, this->emotes_, userId),
        this->lookupBadge(this->badgeMap2_, this->emotes2_, userId),
        this->lookupBadge(this->badgeMap3_, this->emotes3_, userId),
    };
}

EmotePtr HomiesBadges::lookupBadge(const BadgeMap &badgeMap,
                                   const BadgeStorage &emotes,
                                   const QString &userId) const
{
    const auto it = badgeMap.find(userId);
    if (it != badgeMap.end() && it->second >= 0 &&
        it->second < static_cast<int>(emotes.size()))
    {
        return emotes.at(it->second);
    }

    return nullptr;
}

}  // namespace chatterino

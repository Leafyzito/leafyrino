// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/chatsen/ChatsenBadges.hpp"

#include "common/Literals.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/ImageSet.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringBuilder>
#include <QTimer>

#include <unordered_map>

namespace chatterino {

using namespace chatterino::literals;

namespace {

constexpr QSize BADGE_BASE_SIZE(18, 18);

QString badgeTierUrl(const QString &url, int tier)
{
    return url + u"?chatsen-tier="_s + QString::number(tier);
}

ImageSet createBadgeImages(const QString &url)
{
    return ImageSet{
        Image::fromUrl(Url{badgeTierUrl(url, 1)}, 1.0, BADGE_BASE_SIZE),
        Image::fromUrl(Url{badgeTierUrl(url, 2)}, 0.5, BADGE_BASE_SIZE * 2),
        Image::fromUrl(Url{badgeTierUrl(url, 4)}, 0.25, BADGE_BASE_SIZE * 4),
    };
}

const std::unordered_map<QString, int> CHATSEN_BADGE_PRIORITY = {
    {u"developer"_s, 0},
    {u"early_supporter"_s, 1},
    {u"performance_artist"_s, 2},
    {u"early_bird"_s, 3},
    {u"relaxo"_s, 4},
    {u"patreon_tier4"_s, 5},
    {u"patreon_tier3s"_s, 6},
    {u"patreon_tier3"_s, 7},
    {u"patreon_tier2s"_s, 8},
    {u"patreon_tier2"_s, 9},
    {u"patreon_tier1s"_s, 10},
    {u"patreon_tier1"_s, 11},
};

int priorityForBadge(const QString &badgeName)
{
    const auto it = CHATSEN_BADGE_PRIORITY.find(badgeName);
    return it != CHATSEN_BADGE_PRIORITY.end() ? it->second : 999;
}

}  // namespace

ChatsenBadges::ChatsenBadges()
{
    QTimer::singleShot(3500, [this] {
        this->load();
    });
}

std::optional<EmotePtr> ChatsenBadges::getBadge(const QString &userID) const
{
    std::shared_lock lock(this->mutex_);

    const auto it = this->badgeMap_.find(userID);
    if (it != this->badgeMap_.end())
    {
        return it->second;
    }
    return std::nullopt;
}

void ChatsenBadges::load()
{
    NetworkRequest(
        u"https://raw.githubusercontent.com/chatsen/resources/master/assets/data.json"_s)
        .concurrent()
        .timeout(10000)
        .onSuccess([this](const NetworkResult &result) {
            const auto root = result.parseJson();

            // Passo 1: montar catálogo nome → EmotePtr
            const auto badgesArr = root[u"badges"_s].toArray();
            std::unordered_map<QString, EmotePtr> catalog;
            catalog.reserve(static_cast<size_t>(badgesArr.size()));

            for (const auto &val : badgesArr)
            {
                const auto obj = val.toObject();
                const auto name = obj[u"name"_s].toString();
                const auto title = obj[u"title"_s].toString();
                const auto image = obj[u"image"_s].toString();

                if (name.isEmpty() || image.isEmpty())
                {
                    continue;
                }

                const QString tooltip =
                    title.isEmpty() ? name : title;

                auto emote = Emote{
                    .name = EmoteName{u"chatsen:" % name},
                    .images = createBadgeImages(image),
                    .tooltip = Tooltip{tooltip},
                    .homePage = Url{},
                    .id = EmoteId{name},
                };
                catalog[name] =
                    std::make_shared<const Emote>(std::move(emote));
            }

            // Passo 2: para cada usuário, selecionar o badge de maior prioridade
            const auto usersArr = root[u"users"_s].toArray();
            std::unordered_map<QString, EmotePtr> map;
            map.reserve(static_cast<size_t>(usersArr.size()));

            for (const auto &userVal : usersArr)
            {
                const auto userObj = userVal.toObject();
                const auto userId = userObj[u"id"_s].toString();
                if (userId.isEmpty())
                {
                    continue;
                }

                const auto userBadges = userObj[u"badges"_s].toArray();

                EmotePtr bestEmote;
                int bestPriority = INT_MAX;

                for (const auto &badgeVal : userBadges)
                {
                    const auto badgeObj = badgeVal.toObject();
                    const auto badgeName =
                        badgeObj[u"badgeName"_s].toString();

                    const int priority = priorityForBadge(badgeName);
                    if (priority >= bestPriority)
                    {
                        continue;
                    }

                    const auto emoteIt = catalog.find(badgeName);
                    if (emoteIt == catalog.end())
                    {
                        continue;
                    }

                    bestPriority = priority;
                    bestEmote = emoteIt->second;
                }

                if (bestEmote)
                {
                    map[userId] = std::move(bestEmote);
                }
            }

            const auto count = map.size();

            {
                std::unique_lock lock(this->mutex_);
                this->badgeMap_ = std::move(map);
            }

            qCDebug(chatterinoApp)
                << "[Chatsen] Loaded badges for" << count << "users";
        })
        .onError([](const NetworkResult &result) {
            qCWarning(chatterinoApp)
                << "[Chatsen] Failed to load badges:"
                << result.formatError();
        })
        .execute();
}

}  // namespace chatterino
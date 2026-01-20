// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/homies/HomiesBadges.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/Outcome.hpp"
#include "common/QLogging.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QUrl>

namespace chatterino {

HomiesBadges::HomiesBadges()
{
    this->loadHomiesBadges();
}

std::optional<EmotePtr> HomiesBadges::getBadge(const UserId &id)
{
    std::shared_lock lock(this->mutex_);

    auto it = this->badgeMap.find(id.string);
    if (it != this->badgeMap.end())
    {
        return this->emotes[it->second];
    }
    return std::nullopt;
}

std::optional<EmotePtr> HomiesBadges::getBadge2(const UserId &id)
{
    std::shared_lock lock(this->mutex_);

    auto it = this->badgeMap2.find(id.string);
    if (it != this->badgeMap2.end())
    {
        return this->emotes2[it->second];
    }
    return std::nullopt;
}

std::optional<EmotePtr> HomiesBadges::getBadge3(const UserId &id)
{
    std::shared_lock lock(this->mutex_);

    auto it = this->badgeMap3.find(id.string);
    if (it != this->badgeMap3.end())
    {
        return this->emotes3[it->second];
    }
    return std::nullopt;
}

void HomiesBadges::loadHomiesBadges()
{
    this->badgeMap.clear();
    this->badgeMap2.clear();
    this->badgeMap3.clear();
    this->emotes.clear();
    this->emotes2.clear();
    this->emotes3.clear();

    static QUrl url("https://chatterinohomies.com/api/badges/list");

    NetworkRequest(url)
        .concurrent()
        .onSuccess([this](auto result) -> Outcome {
            auto jsonRoot = result.parseJson();

            if (!jsonRoot.contains("badges"))
            {
                qCWarning(chatterinoNetwork)
                    << "[HomiesBadges] Response missing 'badges' field from"
                    << url.toString();
                return Failure;
            }

            auto badgesArray = jsonRoot.value("badges").toArray();

            std::unique_lock lock(this->mutex_);

            int index = 0;
            for (const auto &jsonBadge_ : badgesArray)
            {
                auto jsonBadge = jsonBadge_.toObject();
                auto userId = jsonBadge.value("userId").toString();

                auto emote = Emote{
                    EmoteName{},
                    ImageSet{Url{jsonBadge.value("image1").toString()},
                             Url{jsonBadge.value("image2").toString()},
                             Url{jsonBadge.value("image3").toString()}},
                    Tooltip{jsonBadge.value("tooltip").toString()}, Url{}};

                this->emotes.push_back(
                    std::make_shared<const Emote>(std::move(emote)));

                this->badgeMap[userId] = index;
                ++index;
            }
            return Success;
        })
        .execute();

    static QUrl url2("https://itzalex.github.io/badges");

    NetworkRequest(url2)
        .concurrent()
        .onSuccess([this](auto result) -> Outcome {
            auto jsonRoot = result.parseJson();

            std::unique_lock lock(this->mutex_);

            int index = 0;
            for (const auto &jsonBadge_ : jsonRoot.value("badges").toArray())
            {
                auto jsonBadge = jsonBadge_.toObject();
                auto emote = Emote{
                    EmoteName{},
                    ImageSet{Url{jsonBadge.value("image1").toString()},
                             Url{jsonBadge.value("image2").toString()},
                             Url{jsonBadge.value("image3").toString()}},
                    Tooltip{jsonBadge.value("tooltip").toString()}, Url{}};

                this->emotes2.push_back(
                    std::make_shared<const Emote>(std::move(emote)));

                for (const auto &user : jsonBadge.value("users").toArray())
                {
                    auto userId = user.toString();
                    this->badgeMap2[userId] = index;
                }
                ++index;
            }
            return Success;
        })
        .execute();

    static QUrl url3("https://itzalex.github.io/badges2");

    NetworkRequest(url3)
        .concurrent()
        .onSuccess([this](auto result) -> Outcome {
            auto jsonRoot = result.parseJson();

            std::unique_lock lock(this->mutex_);

            int index = 0;
            for (const auto &jsonBadge_ : jsonRoot.value("badges").toArray())
            {
                auto jsonBadge = jsonBadge_.toObject();
                auto emote = Emote{
                    EmoteName{},
                    ImageSet{Url{jsonBadge.value("image1").toString()},
                             Url{jsonBadge.value("image2").toString()},
                             Url{jsonBadge.value("image3").toString()}},
                    Tooltip{jsonBadge.value("tooltip").toString()}, Url{}};

                this->emotes3.push_back(
                    std::make_shared<const Emote>(std::move(emote)));

                for (const auto &user : jsonBadge.value("users").toArray())
                {
                    auto userId = user.toString();
                    this->badgeMap3[userId] = index;
                }
                ++index;
            }
            return Success;
        })
        .execute();
}

}  // namespace chatterino

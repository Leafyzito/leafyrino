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
#include <QJsonObject>
#include <QJsonValue>
#include <QUrl>

namespace chatterino {

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
    this->badgeMap.clear();

    static QUrl url("https://api.folhinhabot.com/plus");

    NetworkRequest(url)
        .concurrent()
        .onSuccess([this](auto result) -> Outcome {
            auto jsonRoot = result.parseJson();

            if (!jsonRoot.contains("plus"))
            {
                qCWarning(chatterinoNetwork)
                    << "[FolhinhaBadges] Response missing 'plus' field from"
                    << url.toString();
                return Failure;
            }

            std::unique_lock lock(this->mutex_);

            // Process plus users
            if (jsonRoot.contains("plus"))
            {
                auto plusArray = jsonRoot.value("plus").toArray();
                for (const auto &userValue : plusArray)
                {
                    auto userObj = userValue.toObject();
                    auto userId = userObj.value("userid").toString();

                    if (userId.isEmpty())
                    {
                        continue;
                    }

                    bool isFounder = userObj.value("isFounder").toBool();
                    QString tooltip;
                    QString badgePrefix;
                    if (isFounder)
                    {
                        tooltip = "FolhinhaBot Plus (Founder)";
                        badgePrefix = "founder";
                    }
                    else
                    {
                        tooltip = "FolhinhaBot Plus";
                        badgePrefix = "sub";
                    }

                    auto emote = Emote{
                        .name = EmoteName{u"folhinha:" % tooltip},
                        .images =
                            ImageSet{
                                Image::fromUrl(
                                    Url{"http://folhinhabot.com/badges/" %
                                        badgePrefix % "1"},
                                    1.0, QSize(18, 18)),
                                Image::fromUrl(
                                    Url{"http://folhinhabot.com/badges/" %
                                        badgePrefix % "2"},
                                    0.5, QSize(36, 36)),
                                Image::fromUrl(
                                    Url{"http://folhinhabot.com/badges/" %
                                        badgePrefix % "3"},
                                    0.25, QSize(72, 72)),
                            },
                        .tooltip = Tooltip{tooltip},
                        .homePage = Url{},
                    };

                    this->badgeMap[userId] =
                        std::make_shared<const Emote>(std::move(emote));
                }
            }

            // Process supporters
            // TODO: Re-enable supporter badges when ready
            /*
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

                    QString tooltip = "FolhinhaBot Supporter";

                    auto emote = Emote{
                        .name = EmoteName{u"folhinha:" % tooltip},
                        .images =
                            ImageSet{
                                Image::fromUrl(
                                    Url{"http://folhinhabot.com/badges/sub1"},
                                    1.0, QSize(18, 18)),
                                Image::fromUrl(
                                    Url{"http://folhinhabot.com/badges/sub2"},
                                    0.5, QSize(36, 36)),
                                Image::fromUrl(
                                    Url{"http://folhinhabot.com/badges/sub3"},
                                    0.25, QSize(72, 72)),
                            },
                        .tooltip = Tooltip{tooltip},
                        .homePage = Url{},
                    };

                    this->badgeMap[userId] =
                        std::make_shared<const Emote>(std::move(emote));
                }
            }
            */

            return Success;
        })
        .execute();
}

}  // namespace chatterino

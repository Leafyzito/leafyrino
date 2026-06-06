// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/bttv/BttvBadges.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/ImageSet.hpp"
#include "singletons/WindowManager.hpp"
#include "util/PostToThread.hpp"

using namespace Qt::Literals::StringLiterals;

namespace chatterino {

namespace {

using namespace chatterino::literals;

constexpr QSize BADGE_BASE_SIZE(18, 18);

QString badgeTierUrl(const QString &url, int tier)
{
    return url + u"?bttv-tier="_s + QString::number(tier);
}

ImageSet createBadgeImages(const QString &id)
{
    return ImageSet{
        Image::fromUrl(Url{badgeTierUrl(id, 1)}, 1.0, BADGE_BASE_SIZE),
        Image::fromUrl(Url{badgeTierUrl(id, 2)}, 0.5, BADGE_BASE_SIZE * 2),
        Image::fromUrl(Url{badgeTierUrl(id, 4)}, 0.25, BADGE_BASE_SIZE * 4),
    };
}

}  // namespace

QString BttvBadges::idForBadge(const QJsonObject &badgeJson) const
{
    const auto url = badgeJson[u"url"_s].toString();
    if (!url.isEmpty())
    {
        return url;
    }

    return badgeJson[u"svg"_s].toString();
}

EmotePtr BttvBadges::createBadge(const QString &id,
                                 const QJsonObject &badgeJson) const
{
    const int badgeType = badgeJson[u"type"_s].toInt(0);

    QString emoteName;
    QString tooltip;

    switch (badgeType)
    {
        case 1:
            emoteName = u"betterttv:developer"_s;
            tooltip = "BTTV Developer";
            break;
        case 2:
            emoteName = u"betterttv:support-volunteer"_s;
            tooltip = "BTTV Support Volunteer";
            break;
        case 3:
            emoteName = u"betterttv:emote-approver"_s;
            tooltip = "BTTV Emote Approver";
            break;
        case 4:
            emoteName = u"betterttv:translator"_s;
            tooltip = "BTTV Translator";
            break;
        default:
            emoteName = u"betterttv:pro"_s;
            tooltip = "BTTV Pro";
            break;
    }

    const auto description = badgeJson[u"description"_s].toString();
    if (!description.isEmpty())
    {
        tooltip = description;
    }

    auto emote = Emote{
        .name = EmoteName{emoteName},
        .images = createBadgeImages(id),
        .tooltip = Tooltip{tooltip},
        .homePage = Url{},
        .id = EmoteId{id},
    };
    return std::make_shared<const Emote>(std::move(emote));
}

void BttvBadges::load()
{
    NetworkRequest(u"https://api.betterttv.net/3/cached/badges/twitch"_s)
        .concurrent()
        .timeout(10000)
        .onSuccess([this](const NetworkResult &result) {
            const auto users = result.parseJsonArray();
            size_t assigned = 0;

            for (const auto &userValue : users)
            {
                const auto userObject = userValue.toObject();
                const auto providerId = userObject[u"providerId"_s].toString();
                const auto badgeObject = userObject[u"badge"_s].toObject();

                if (providerId.isEmpty() || badgeObject.isEmpty())
                {
                    continue;
                }

                const auto badgeID = this->registerBadge(badgeObject);
                if (badgeID.isEmpty())
                {
                    continue;
                }

                this->assignBadgeToUser(badgeID, UserId{providerId});
                ++assigned;
            }

            if (assigned > 0)
            {
                runInGuiThread([] {
                    if (auto *windows = tryGetApp()->getWindows())
                    {
                        windows->invalidateChannelViewBuffers();
                    }
                });
            }

            qCDebug(chatterinoBttv)
                << "Loaded BTTV staff badges for" << assigned << "users";
        })
        .onError([](const NetworkResult &result) {
            qCWarning(chatterinoBttv)
                << "Failed to load BTTV staff badges:" << result.formatError();
        })
        .execute();
}

}  // namespace chatterino

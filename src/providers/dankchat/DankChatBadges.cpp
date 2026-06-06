// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/dankchat/DankChatBadges.hpp"

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
#include <unordered_set>

namespace chatterino {

using namespace chatterino::literals;

namespace {

constexpr QSize BADGE_BASE_SIZE(18, 18);

ImageSet createBadgeImages(const QString &url)
{
    return ImageSet{
        Image::fromUrl(Url{url}, 1.0, BADGE_BASE_SIZE),
        Image::fromUrl(Url{url}, 0.5, BADGE_BASE_SIZE * 2),
        Image::fromUrl(Url{url}, 0.25, BADGE_BASE_SIZE * 4),
    };
}

// Badge com este tipo é filtrado — o cliente oficial do DankChat também remove ele
const QString DANKCHAT_FILTERED_TYPE = u"DankChat Top Supporter"_s;

}  // namespace

DankChatBadges::DankChatBadges()
{
    QTimer::singleShot(3500, [this] {
        this->load();
    });
}

std::optional<EmotePtr> DankChatBadges::getBadge(const QString &userID) const
{
    std::shared_lock lock(this->mutex_);

    const auto it = this->badgeMap_.find(userID);
    if (it != this->badgeMap_.end())
    {
        return it->second;
    }
    return std::nullopt;
}

void DankChatBadges::load()
{
    NetworkRequest(u"https://flxrs.com/api/badges"_s)
        .concurrent()
        .timeout(10000)
        .onSuccess([this](const NetworkResult &result) {
            const auto arr = result.parseJsonArray();

            std::unordered_map<QString, EmotePtr> typeToEmote;
            std::unordered_map<QString, std::unordered_set<QString>>
                typeToUsers;

            for (const auto &val : arr)
            {
                const auto obj = val.toObject();
                const auto type = obj[u"type"_s].toString();
                const auto url = obj[u"url"_s].toString();

                if (type.isEmpty() || url.isEmpty())
                {
                    continue;
                }

                if (type == DANKCHAT_FILTERED_TYPE)
                {
                    continue;
                }

                if (!typeToEmote.count(type))
                {
                    auto emote = Emote{
                        .name = EmoteName{u"dankchat:" % type},
                        .images = createBadgeImages(url),
                        .tooltip = Tooltip{type},
                        .homePage = Url{},
                        .id = EmoteId{url},
                    };
                    typeToEmote[type] =
                        std::make_shared<const Emote>(std::move(emote));
                }

                const auto users = obj[u"users"_s].toArray();
                auto &userSet = typeToUsers[type];
                for (const auto &userVal : users)
                {
                    const auto uid = userVal.toString();
                    if (!uid.isEmpty())
                    {
                        userSet.insert(uid);
                    }
                }
            }

            std::unordered_map<QString, EmotePtr> map;
            for (const auto &[type, emote] : typeToEmote)
            {
                const auto usersIt = typeToUsers.find(type);
                if (usersIt == typeToUsers.end())
                {
                    continue;
                }
                for (const auto &uid : usersIt->second)
                {
                    map.emplace(uid, emote);
                }
            }

            const auto count = map.size();

            {
                std::unique_lock lock(this->mutex_);
                this->badgeMap_ = std::move(map);
            }

            qCDebug(chatterinoApp)
                << "[DankChat] Loaded badges for" << count << "users";
        })
        .onError([](const NetworkResult &result) {
            qCWarning(chatterinoApp)
                << "[DankChat] Failed to load badges:" << result.formatError();
        })
        .execute();
}

}  // namespace chatterino

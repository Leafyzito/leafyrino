#include "providers/ffzap/FfzApBadges.hpp"

#include "common/Literals.hpp"
#include "common/QLogging.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringBuilder>
#include <QTimer>

namespace chatterino {

using namespace chatterino::literals;

namespace {

constexpr QSize BADGE_BASE_SIZE(18, 18);

static const std::unordered_map<QString, QString> FFZAP_HELPERS = {
    {u"26964566"_s, u"FFZ:AP Developer"_s},
    {u"11819690"_s, u"FFZ:AP Helper"_s},
    {u"36442149"_s, u"FFZ:AP Helper"_s},
    {u"29519423"_s, u"FFZ:AP Helper"_s},
    {u"22025290"_s, u"FFZ:AP Helper"_s},
    {u"4867723"_s,  u"FFZ:AP Helper"_s},
};

}  // namespace

FfzApBadges::FfzApBadges()
{
    QTimer::singleShot(3500, [this] {
        this->load();
    });
}

std::optional<FfzApBadges::Badge> FfzApBadges::getBadge(
    const QString &userID) const
{
    std::shared_lock lock(this->mutex_);
    const auto it = this->badgeMap_.find(userID);
    if (it != this->badgeMap_.end())
        return it->second;
    return std::nullopt;
}

void FfzApBadges::load()
{
    NetworkRequest(u"https://api.ffzap.com/v1/supporters"_s)
        .concurrent()
        .timeout(10000)
        .onSuccess([this](const NetworkResult &result) {
            const auto arr = result.parseJsonArray();

            std::unordered_map<QString, Badge> map;

            for (const auto &val : arr)
            {
                const auto obj = val.toObject();
                const auto id = obj[u"id"_s].toString();
                if (id.isEmpty() || id == u"undefined"_s)
                    continue;

                const auto badgeBaseUrl =
                    u"https://api.ffzap.com/v1/user/badge/" % id;

                const auto helperIt = FFZAP_HELPERS.find(id);
                const QString tooltip = helperIt != FFZAP_HELPERS.end()
                                            ? helperIt->second
                                            : u"FFZ:AP Supporter"_s;

                const bool isColored = obj[u"badge_is_colored"_s].toBool();
                const QColor color = isColored
                                         ? QColor(obj[u"badge_color"_s].toString())
                                         : QColor{};

                auto emote = Emote{
                    .name = EmoteName{u"ffzap:" % tooltip},
                    .images =
                        ImageSet{
                            Image::fromUrl(Url{badgeBaseUrl % u"/1"}, 1.0,
                                           BADGE_BASE_SIZE),
                            Image::fromUrl(Url{badgeBaseUrl % u"/2"}, 0.5,
                                           BADGE_BASE_SIZE * 2),
                            Image::fromUrl(Url{badgeBaseUrl % u"/3"}, 0.25,
                                           BADGE_BASE_SIZE * 4),
                        },
                    .tooltip = Tooltip{tooltip},
                    .homePage = Url{},
                    .id = EmoteId{badgeBaseUrl % u"/1"},
                };

                map[id] = Badge{
                    .emote = std::make_shared<const Emote>(std::move(emote)),
                    .color = color,
                };
            }

            {
                std::unique_lock lock(this->mutex_);
                this->badgeMap_ = std::move(map);
            }

            qCDebug(chatterinoApp)
                << "[FFZ:AP] Loaded" << this->badgeMap_.size() << "badges";
        })
        .onError([](const NetworkResult &result) {
            qCWarning(chatterinoApp)
                << "[FFZ:AP] Failed to load badges:"
                << result.formatError();
        })
        .execute();
}

}  // namespace chatterino
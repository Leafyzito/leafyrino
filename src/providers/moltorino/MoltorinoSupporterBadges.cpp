#include "providers/moltorino/MoltorinoSupporterBadges.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "debug/AssertInGuiThread.hpp"
#include "messages/Image.hpp"
#include "singletons/Paths.hpp"
#include "util/PostToThread.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSize>
#include <QTimer>
#include <QVariant>

#include <algorithm>
#include <mutex>

namespace chatterino {

namespace {

using namespace literals;

constexpr auto ENDPOINT = "https://api.moltorino.com/badges";
constexpr auto CACHE_FILE = "moltorino-supporter-badges.json";
constexpr int PASSIVE_REFRESH_THROTTLE_MS = 60000;
constexpr qsizetype MAX_PAYLOAD_BYTES = 8 * 1024 * 1024;
constexpr int MAX_CATEGORIES = 64;
constexpr int MAX_ASSIGNMENTS = 250000;
constexpr QSize BADGE_BASE_SIZE(18, 18);

struct ParsedPayload {
    int version = -1;
    std::unordered_map<QString, std::vector<MoltorinoSupporterBadge>>
        userBadges;
    size_t categoryCount = 0;
    size_t assignmentCount = 0;
};

bool isValidUserId(const QString &value)
{
    if (value.isEmpty() || value.size() > 32)
    {
        return false;
    }

    return std::ranges::all_of(value, [](const QChar ch) {
        return ch.isDigit();
    });
}

QString versionedImageUrl(const QString &url, int version)
{
    if (url.isEmpty() || version < 0 || url.startsWith(u":/"_s))
    {
        return url;
    }

    const auto fragmentIndex = url.indexOf(u'#');
    auto base = fragmentIndex < 0 ? url : url.left(fragmentIndex);
    const auto fragment =
        fragmentIndex < 0 ? QString{} : url.mid(fragmentIndex);

    base += base.contains(u'?') ? u'&' : u'?';
    base += u"mbv="_s + QString::number(version);

    return base + fragment;
}

ImagePtr badgeImage(const QString &url, int version, qreal scale,
                    QSize expectedSize)
{
    if (url.isEmpty())
    {
        return Image::getEmpty();
    }

    return Image::fromUrl(Url{versionedImageUrl(url, version)}, scale,
                          expectedSize);
}

ImageSet badgeImageSet(const QString &image1, const QString &image2,
                       const QString &image3, int version)
{
    const auto badge1x = badgeImage(image1, version, 1.0, BADGE_BASE_SIZE);
    const auto badge2x = badgeImage(image2, version, 0.5, BADGE_BASE_SIZE * 2);
    const auto badge3x = badgeImage(image3, version, 0.25, BADGE_BASE_SIZE * 4);

    return ImageSet{
        badge1x,
        badge2x,
        badge3x,
    };
}

EmotePtr makeBadgeEmote(const QJsonObject &category, int version)
{
    const auto id = category.value("id").toString().trimmed();
    if (id.isEmpty())
    {
        return nullptr;
    }

    const auto images = category.value("images").toObject();
    auto image1 = images.value("1x").toString().trimmed();
    auto image2 = images.value("2x").toString().trimmed();
    auto image3 = images.value("3x").toString().trimmed();
    if (image1.isEmpty())
    {
        image1 = category.value("image1").toString().trimmed();
    }
    if (image2.isEmpty())
    {
        image2 = category.value("image2").toString().trimmed();
    }
    if (image3.isEmpty())
    {
        image3 = category.value("image3").toString().trimmed();
    }

    auto imageSet = badgeImageSet(image1, image2, image3, version);
    if (imageSet.getImage1()->isEmpty())
    {
        return nullptr;
    }

    auto tooltip = category.value("tooltip").toString().trimmed();
    if (tooltip.isEmpty())
    {
        tooltip = id;
    }

    auto emote = Emote{
        .name = EmoteName{u"moltorino:"_s + id},
        .images = std::move(imageSet),
        .tooltip = Tooltip{tooltip},
        .homePage = Url{},
        .id = EmoteId{id},
    };

    return std::make_shared<const Emote>(std::move(emote));
}

void addBadgeAssignment(ParsedPayload &parsed, const QString &userId,
                        const QString &categoryId, const EmotePtr &emote)
{
    auto &badges = parsed.userBadges[userId];
    if (std::ranges::any_of(badges, [&](const auto &badge) {
            return badge.categoryId == categoryId;
        }))
    {
        return;
    }

    badges.emplace_back(MoltorinoSupporterBadge{categoryId, emote});
}

bool parsePayload(const QByteArray &payload, ParsedPayload &parsed)
{
    if (payload.size() > MAX_PAYLOAD_BYTES)
    {
        return false;
    }

    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
    {
        return false;
    }

    const auto root = document.object();
    bool ok = false;
    const auto version = root.value("version").toVariant().toInt(&ok);
    if (!ok || version < 0)
    {
        return false;
    }

    parsed.version = version;

    auto categories = root.value("badges").toArray();
    if (categories.isEmpty() && root.contains("categories"))
    {
        categories = root.value("categories").toArray();
    }

    parsed.userBadges.reserve(std::min<int>(categories.size() * 64, 4096));

    for (const auto &categoryValue : categories)
    {
        if (parsed.categoryCount >= MAX_CATEGORIES ||
            parsed.assignmentCount >= MAX_ASSIGNMENTS)
        {
            break;
        }

        const auto category = categoryValue.toObject();
        const auto categoryId = category.value("id").toString().trimmed();
        if (categoryId.isEmpty())
        {
            continue;
        }

        auto emote = makeBadgeEmote(category, version);
        if (!emote)
        {
            continue;
        }

        ++parsed.categoryCount;

        const auto usersValue = category.value("users");
        if (usersValue.isArray())
        {
            const auto users = usersValue.toArray();
            for (const auto &userValue : users)
            {
                if (parsed.assignmentCount >= MAX_ASSIGNMENTS)
                {
                    break;
                }

                const auto user = userValue.toObject();
                auto userId = user.value("id").toString().trimmed();
                if (userId.isEmpty())
                {
                    userId = user.value("userId").toString().trimmed();
                }
                if (!isValidUserId(userId))
                {
                    continue;
                }

                ++parsed.assignmentCount;

                addBadgeAssignment(parsed, userId, categoryId, emote);
            }
            continue;
        }

        const auto users = usersValue.toObject();
        for (auto it = users.constBegin(); it != users.constEnd(); ++it)
        {
            if (parsed.assignmentCount >= MAX_ASSIGNMENTS)
            {
                break;
            }

            const auto userId = it.key().trimmed();
            if (!isValidUserId(userId))
            {
                continue;
            }

            ++parsed.assignmentCount;

            addBadgeAssignment(parsed, userId, categoryId, emote);
        }
    }

    return true;
}

QString cachePath()
{
    return getApp()->getPaths().cacheFilePath(QString::fromUtf8(CACHE_FILE));
}

}  // namespace

MoltorinoSupporterBadges::MoltorinoSupporterBadges(QObject *parent)
    : QObject(parent)
{
}

void MoltorinoSupporterBadges::initialize()
{
    if (this->initialized_)
    {
        return;
    }

    this->initialized_ = true;
    this->loadCache();
    this->refreshNow();
}

void MoltorinoSupporterBadges::refreshNow()
{
    if (!isGuiThread())
    {
        postToThread([this] {
            this->refreshNow();
        });
        return;
    }

    this->refreshInternal(true, std::nullopt);
}

void MoltorinoSupporterBadges::refreshPassive()
{
    if (!isGuiThread())
    {
        postToThread([this] {
            this->refreshPassive();
        });
        return;
    }

    this->refreshInternal(false, std::nullopt);
}

void MoltorinoSupporterBadges::refreshIfNewer(int version)
{
    if (!isGuiThread())
    {
        postToThread([this, version] {
            this->refreshIfNewer(version);
        });
        return;
    }

    if (version <= this->version_)
    {
        return;
    }

    this->refreshInternal(true, version);
}

std::vector<MoltorinoSupporterBadge> MoltorinoSupporterBadges::getBadges(
    const QString &userId) const
{
    std::shared_lock lock(this->mutex_);
    const auto it = this->userBadges_.find(userId);
    if (it == this->userBadges_.end())
    {
        return {};
    }

    return it->second;
}

void MoltorinoSupporterBadges::refreshInternal(
    bool force, std::optional<int> minimumVersion)
{
    assertInGuiThread();

    if (minimumVersion && *minimumVersion <= this->version_)
    {
        return;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    if (!force && this->lastFetchAttempt_.isValid() &&
        this->lastFetchAttempt_.msecsTo(now) < PASSIVE_REFRESH_THROTTLE_MS)
    {
        return;
    }

    if (this->requestInFlight_)
    {
        this->pendingRefresh_ = true;
        this->pendingForce_ = this->pendingForce_ || force;
        if (minimumVersion && (!this->pendingMinimumVersion_ ||
                               *minimumVersion > *this->pendingMinimumVersion_))
        {
            this->pendingMinimumVersion_ = minimumVersion;
        }
        return;
    }

    this->requestInFlight_ = true;
    this->lastFetchAttempt_ = now;

    NetworkRequest(ENDPOINT)
        .header("Accept", "application/json")
        .timeout(5000)
        .caller(this)
        .onSuccess([this, minimumVersion](const NetworkResult &result) {
            if (minimumVersion && *minimumVersion <= this->version_)
            {
                return;
            }

            if (this->applyPayload(result.getData(), false, minimumVersion))
            {
                this->saveCache(result.getData());
            }
        })
        .onError([](const NetworkResult &result) {
            qCWarning(chatterinoApp)
                << "[Moltorino] Failed to load supporter badges:"
                << result.formatError();
        })
        .finally([this] {
            this->finishRequest();
        })
        .execute();
}

void MoltorinoSupporterBadges::finishRequest()
{
    assertInGuiThread();

    this->requestInFlight_ = false;
    if (!this->pendingRefresh_)
    {
        return;
    }

    const auto force = this->pendingForce_;
    const auto minimumVersion = this->pendingMinimumVersion_;
    this->pendingRefresh_ = false;
    this->pendingForce_ = false;
    this->pendingMinimumVersion_.reset();

    QTimer::singleShot(0, this, [this, force, minimumVersion] {
        this->refreshInternal(force, minimumVersion);
    });
}

void MoltorinoSupporterBadges::loadCache()
{
    QFile file(cachePath());
    if (!file.open(QIODevice::ReadOnly))
    {
        return;
    }

    this->applyPayload(file.readAll(), true);
}

void MoltorinoSupporterBadges::saveCache(const QByteArray &payload) const
{
    QSaveFile file(cachePath());
    if (!file.open(QIODevice::WriteOnly))
    {
        return;
    }

    file.write(payload);
    file.commit();
}

bool MoltorinoSupporterBadges::applyPayload(const QByteArray &payload,
                                            bool fromCache,
                                            std::optional<int> minimumVersion)
{
    ParsedPayload parsed;
    if (!parsePayload(payload, parsed))
    {
        if (!fromCache)
        {
            qCWarning(chatterinoApp)
                << "[Moltorino] Ignoring malformed supporter badge payload.";
        }
        return false;
    }

    if (minimumVersion && parsed.version < *minimumVersion)
    {
        if (!fromCache)
        {
            qCWarning(chatterinoApp)
                << "[Moltorino] Ignoring stale badge payload version"
                << parsed.version << "while waiting for" << *minimumVersion;
        }
        return false;
    }

    if (parsed.version < this->version_)
    {
        return false;
    }

    {
        std::unique_lock lock(this->mutex_);
        this->version_ = parsed.version;
        this->userBadges_ = std::move(parsed.userBadges);
    }

    qCDebug(chatterinoApp) << "[Moltorino] Loaded supporter badges:"
                           << parsed.userBadges.size() << "users across"
                           << parsed.categoryCount << "categories from"
                           << parsed.assignmentCount << "assignments in"
                           << (fromCache ? "cache" : "network") << "version"
                           << this->version_;

    return true;
}

}  // namespace chatterino

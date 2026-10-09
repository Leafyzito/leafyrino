// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/bluzyrino/BluzyrinoBadges.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "singletons/Paths.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <mutex>

namespace chatterino {
namespace {

constexpr auto BADGES_URL =
    "https://bluzyrino-badge-registry.blu901-55.workers.dev/v1/badges";
constexpr qsizetype MAX_PAYLOAD_SIZE = 2 * 1024 * 1024;
constexpr auto REFRESH_INTERVAL = 30 * 60 * 1000;

QString cachePath()
{
    return getApp()->getPaths().cacheFilePath(
        QStringLiteral("bluzyrino-badges.json"));
}

bool validImageUrl(const QString &value)
{
    const QUrl url(value, QUrl::StrictMode);
    return value.size() <= 2048 && url.isValid() &&
           url.scheme() == QStringLiteral("https") &&
           url.host() ==
               QStringLiteral(
                   "bluzyrino-badge-registry.blu901-55.workers.dev") &&
           url.userInfo().isEmpty() && url.port(-1) == -1 &&
           !url.hasFragment() &&
           url.path().startsWith(QStringLiteral("/badges/"));
}

bool validUserId(const QString &id)
{
    return !id.isEmpty() && id.size() <= 20 && id.front() != QLatin1Char('0') &&
           std::ranges::all_of(id, [](QChar c) {
               return c >= QLatin1Char('0') && c <= QLatin1Char('9');
           });
}

}  // namespace

void BluzyrinoBadges::initialize()
{
    QFile cache(cachePath());
    if (cache.open(QIODevice::ReadOnly))
    {
        this->applyPayload(cache.read(MAX_PAYLOAD_SIZE + 1));
    }
    QTimer::singleShot(3500, this, [this] {
        this->requestBadges();
    });
}

std::vector<std::shared_ptr<const Emote>> BluzyrinoBadges::getBadges(
    const UserId &userID) const
{
    std::unique_lock lock(this->mutex_);
    const auto found = this->users_.find(userID.string);
    if (found == this->users_.end())
    {
        return {};
    }
    auto &badge = this->catalog_[*std::ranges::min_element(found->second)];
    if (!badge.emote)
    {
        badge.emote = std::make_shared<const Emote>(Emote{
            .name = EmoteName{QStringLiteral("bluzyrino:") + badge.id},
            .images =
                ImageSet{
                    Image::fromUrl(Url{badge.urls[0]}, 1.0, QSize(18, 18)),
                    Image::fromUrl(Url{badge.urls[1]}, 0.5, QSize(36, 36)),
                    Image::fromUrl(Url{badge.urls[2]}, 0.25, QSize(72, 72)),
                },
            .tooltip = Tooltip{badge.tooltip.toHtmlEscaped()},
            .homePage = Url{},
            .id = EmoteId{badge.urls[0]},
        });
    }
    return {badge.emote};
}

bool BluzyrinoBadges::applyPayload(const QByteArray &payload)
{
    if (payload.isEmpty() || payload.size() > MAX_PAYLOAD_SIZE)
    {
        return false;
    }
    const auto document = QJsonDocument::fromJson(payload);
    const auto root = document.object();
    if (!document.isObject() || root.value("version").toInt() != 1 ||
        !root.value("catalog").isArray() || !root.value("badges").isArray())
    {
        return false;
    }
    const auto entries = root.value("catalog").toArray();
    const auto assignments = root.value("badges").toArray();
    if (entries.size() > 128 || assignments.size() > entries.size())
    {
        return false;
    }
    std::vector<Badge> catalog;
    boost::unordered_flat_map<QString, size_t> indices;
    for (const auto &value : entries)
    {
        const auto object = value.toObject();
        Badge badge{
            .id = object.value("id").toString(),
            .tooltip = object.value("tooltip").toString(),
            .urls = {object.value("image_url_1x").toString(),
                     object.value("image_url_2x").toString(),
                     object.value("image_url_4x").toString()},
        };
        if (badge.id.isEmpty() || badge.id.size() > 64 ||
            badge.tooltip.isEmpty() || badge.tooltip.size() > 256 ||
            !std::ranges::all_of(badge.urls, validImageUrl) ||
            !indices.emplace(badge.id, catalog.size()).second)
        {
            return false;
        }
        catalog.push_back(std::move(badge));
    }
    boost::unordered_flat_map<QString, std::vector<size_t>> users;
    size_t total = 0;
    std::vector<bool> assigned(catalog.size());

    for (const auto &value : assignments)
    {
        const auto object = value.toObject();
        const auto found = indices.find(object.value("id").toString());
        if (found == indices.end() || !object.value("users").isArray() ||
            assigned[found->second])
        {
            return false;
        }
        assigned[found->second] = true;
        for (const auto &user : object.value("users").toArray())
        {
            const auto id = user.toString();
            if (++total > 50000 || !validUserId(id))
            {
                return false;
            }
            auto &badges = users[id];
            if (std::ranges::find(badges, found->second) == badges.end())
            {
                if (badges.size() >= 16)
                {
                    return false;
                }
                badges.push_back(found->second);
            }
        }
    }
    std::unique_lock lock(this->mutex_);

    for (auto &badge : catalog)
    {
        const auto old =
            std::ranges::find(this->catalog_, badge.id, &Badge::id);
        if (old != this->catalog_.end() && old->urls == badge.urls &&
            old->tooltip == badge.tooltip)
        {
            badge.emote = old->emote;
        }
    }
    this->catalog_ = std::move(catalog);
    this->users_ = std::move(users);
    return true;
}

void BluzyrinoBadges::requestBadges()
{
    if (this->requestInFlight_)
    {
        return;
    }
    this->requestInFlight_ = true;
    NetworkRequest(QUrl(QString::fromLatin1(BADGES_URL)))
        .header("Accept", "application/json")
        .timeout(8000)
        .caller(this)
        .onSuccess([this](const NetworkResult &result) {
            const auto payload = result.getData();
            if (!this->applyPayload(payload))
            {
                qCWarning(chatterinoApp)
                    << "[Bluzyrino] Ignoring an invalid badge list.";
                this->scheduleRefresh(false);
                return;
            }
            QSaveFile file(cachePath());
            if (file.open(QIODevice::WriteOnly) &&
                file.write(payload) == payload.size())
            {
                file.commit();
            }
            this->badgesUpdated.invoke();
            this->scheduleRefresh(true);
        })
        .onError([this](const NetworkResult &result) {
            qCWarning(chatterinoApp)
                << "[Bluzyrino] Badge request failed:" << result.formatError();
            this->scheduleRefresh(false);
        })
        .finally([this] {
            this->requestInFlight_ = false;
        })
        .execute();
}

void BluzyrinoBadges::scheduleRefresh(bool success)
{
    this->failures_ = success ? 0 : std::min(this->failures_ + 1, 3);
    auto delay = REFRESH_INTERVAL;
    if (this->failures_ == 1)
    {
        delay = 30000;
    }
    else if (this->failures_ == 2)
    {
        delay = 120000;
    }
    QTimer::singleShot(delay, this, [this] {
        this->requestBadges();
    });
}

}  // namespace chatterino

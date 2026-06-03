// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/UniqueAccess.hpp"
#include "util/QStringHash.hpp"

#include <pajlada/signals/signal.hpp>
#include <QIcon>
#include <QJsonObject>
#include <QMap>
#include <QString>

#include <memory>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <unordered_map>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

class Settings;
class Paths;
class Image;
class DisplayBadge;

class TwitchBadges
{
    using QIconPtr = std::shared_ptr<QIcon>;
    using ImagePtr = std::shared_ptr<Image>;
    using BadgeIconCallback = std::function<void(QString, const QIconPtr)>;

public:

    std::optional<EmotePtr> badge(const QString &set,
                                  const QString &version) const;

    std::optional<EmotePtr> badge(const QString &set) const;

    void getBadgeIcon(const QString &name, BadgeIconCallback callback);
    void getBadgeIcon(const DisplayBadge &badge, BadgeIconCallback callback);
    void getBadgeIcons(const QList<DisplayBadge> &badges,
                       BadgeIconCallback callback);

    void loadTwitchBadges();

    void loadLocalBadges();

private:
    void loaded();
    void loadEmoteImage(const QString &name, const ImagePtr &image,
                        BadgeIconCallback &&callback);

    std::shared_mutex badgesMutex_;
    QMap<QString, QIconPtr> badgesMap_;

    std::mutex queueMutex_;
    std::queue<QPair<QString, BadgeIconCallback>> callbackQueue_;

    std::shared_mutex loadedMutex_;
    bool loaded_ = false;

    UniqueAccess<
        std::unordered_map<QString, std::unordered_map<QString, EmotePtr>>>
        badgeSets_;
};

}

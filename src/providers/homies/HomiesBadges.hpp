// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/ImageSet.hpp"

#include <array>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <atomic>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

class HomiesBadges final
{
public:
    HomiesBadges();

    void startLoading();
    void loadHomiesBadges();

    std::array<EmotePtr, 3> getBadges(const QString &userId) const;

private:
    void queueRefresh();

    EmotePtr lookupBadge(const std::unordered_map<QString, int> &badgeMap,
                         const std::vector<EmotePtr> &emotes,
                         const QString &userId) const;

    std::atomic_bool refreshQueued_{false};
    std::atomic_bool loadStarted_{false};
    std::atomic_bool hasLoadedBadges_{false};
    mutable std::shared_mutex mutex_;
    std::unordered_map<QString, int> badgeMap_;
    std::vector<EmotePtr> emotes_;

    std::unordered_map<QString, int> badgeMap2_;
    std::vector<EmotePtr> emotes2_;

    std::unordered_map<QString, int> badgeMap3_;
    std::vector<EmotePtr> emotes3_;
};

}  // namespace chatterino

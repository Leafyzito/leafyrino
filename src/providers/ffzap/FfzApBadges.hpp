// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QColor>
#include <QHashFunctions>
#include <QString>

#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

class FfzApBadges final
{
public:
    FfzApBadges();

    struct Badge {
        EmotePtr emote;
        QColor color;
    };

    std::optional<Badge> getBadge(const QString &userID) const;

private:
    void load();

    mutable std::shared_mutex mutex_;
    std::unordered_map<QString, Badge> badgeMap_;
};

}  // namespace chatterino
// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QHashFunctions>
#include <QString>

#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

class DankChatBadges final
{
public:
    DankChatBadges();

    std::optional<EmotePtr> getBadge(const QString &userID) const;

private:
    void load();

    mutable std::shared_mutex mutex_;
    std::unordered_map<QString, EmotePtr> badgeMap_;
};

}  // namespace chatterino

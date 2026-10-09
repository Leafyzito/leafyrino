// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"
#include "util/QStringHash.hpp"  // IWYU pragma: keep

#include <boost/unordered/unordered_flat_map.hpp>
#include <pajlada/signals/signal.hpp>
#include <QByteArray>
#include <QObject>
#include <QString>

#include <array>
#include <memory>
#include <shared_mutex>
#include <vector>

namespace chatterino {

struct Emote;

class BluzyrinoBadges final : public QObject
{
public:
    void initialize();
    std::vector<std::shared_ptr<const Emote>> getBadges(
        const UserId &userID) const;

    pajlada::Signals::NoArgSignal badgesUpdated;

private:
    struct Badge {
        QString id;
        QString tooltip;
        std::array<QString, 3> urls;
        std::shared_ptr<const Emote> emote;
    };

    bool applyPayload(const QByteArray &payload);
    void requestBadges();
    void scheduleRefresh(bool success);

    mutable std::shared_mutex mutex_;
    mutable std::vector<Badge> catalog_;
    boost::unordered_flat_map<QString, std::vector<size_t>> users_;
    bool requestInFlight_ = false;
    int failures_ = 0;
};

}  // namespace chatterino

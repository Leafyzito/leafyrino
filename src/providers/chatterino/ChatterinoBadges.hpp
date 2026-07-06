#pragma once

#include "common/Aliases.hpp"

#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

class IChatterinoBadges
{
public:
    IChatterinoBadges() = default;
    virtual ~IChatterinoBadges() = default;

    IChatterinoBadges(const IChatterinoBadges &) = delete;
    IChatterinoBadges(IChatterinoBadges &&) = delete;
    IChatterinoBadges &operator=(const IChatterinoBadges &) = delete;
    IChatterinoBadges &operator=(IChatterinoBadges &&) = delete;

    virtual std::optional<EmotePtr> getBadge(const UserId &id) = 0;
    virtual EmotePtr getKickBadge(uint64_t kickID) = 0;
    virtual void setKickMapping(const QString &twitchID, uint64_t kickID) = 0;
};

class ChatterinoBadges : public IChatterinoBadges
{
public:
    ChatterinoBadges();

    std::optional<EmotePtr> getBadge(const UserId &id) override;

    EmotePtr getKickBadge(uint64_t kickID) override;

    void setKickMapping(const QString &twitchID, uint64_t kickID) override;

private:
    void loadChatterinoBadges();

    std::shared_mutex mutex_;

    std::unordered_map<QString, int> badgeMap;

    std::unordered_map<uint64_t, int> kickMapping;

    /**
     * Keeps a list of badges.
     * Indexes in here are referred to by badgeMap
     */
    std::vector<EmotePtr> emotes;
};

}  // namespace chatterino

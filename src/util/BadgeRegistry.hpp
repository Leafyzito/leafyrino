#pragma once

#include "common/Aliases.hpp"

#include <QJsonObject>

#include <shared_mutex>
#include <span>
#include <variant>

namespace chatterino {

namespace seventv::eventapi {
struct TwitchUser;
struct KickUser;
using User = std::variant<TwitchUser, KickUser>;
}

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

class BadgeRegistry
{
public:
    virtual ~BadgeRegistry() = default;

    std::optional<EmotePtr> getBadge(const UserId &id) const;

    std::optional<EmotePtr> getKickBadge(uint64_t id) const;

    void assignBadgeToUser(const QString &badgeID, const UserId &userID);

    void assignBadgeToUsers(const QString &badgeID,
                            std::span<const seventv::eventapi::User> users);

    void clearBadgeFromUser(const QString &badgeID, const UserId &userID);

    void clearBadgeFromUsers(const QString &badgeID,
                             std::span<const seventv::eventapi::User> users);

    QString registerBadge(const QJsonObject &badgeJson);

protected:
    BadgeRegistry() = default;

    virtual QString idForBadge(const QJsonObject &badgeJson) const = 0;
    virtual EmotePtr createBadge(const QString &id,
                                 const QJsonObject &badgeJson) const = 0;

private:

    mutable std::shared_mutex mutex_;

    std::unordered_map<QString, EmotePtr> badgeMap_;

    std::unordered_map<uint64_t, EmotePtr> kickBadgeMap_;

    std::unordered_map<QString, EmotePtr> knownBadges_;
};

}

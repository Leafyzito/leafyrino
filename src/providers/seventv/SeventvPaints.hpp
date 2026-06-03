#pragma once

#include "providers/seventv/paints/Paint.hpp"

#include <QJsonArray>
#include <QString>

#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <variant>

namespace chatterino {

namespace seventv::eventapi {
struct TwitchUser;
struct KickUser;
using User = std::variant<TwitchUser, KickUser>;
}

class SeventvPaints
{
public:
    SeventvPaints();

    void addPaint(const QJsonObject &paintJson);
    void assignPaintToUsers(const QString &paintID,
                            std::span<const seventv::eventapi::User> users);
    void clearPaintFromUsers(const QString &paintID,
                             std::span<const seventv::eventapi::User> users);

    std::shared_ptr<Paint> getPaint(const QString &userName, bool kick) const;

private:

    mutable std::shared_mutex mutex_;

    std::unordered_map<QString, std::shared_ptr<Paint>> kickPaintMap_;

    std::unordered_map<QString, std::shared_ptr<Paint>> twitchPaintMap_;

    std::unordered_map<QString, std::shared_ptr<Paint>> knownPaints_;
};

}

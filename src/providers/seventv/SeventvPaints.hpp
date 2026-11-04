#pragma once

#include "providers/seventv/paints/Paint.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QJsonArray>
#include <QString>

#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace chatterino {

namespace seventv::eventapi {
struct TwitchUser;
struct KickUser;
using User = std::variant<TwitchUser, KickUser>;
}  // namespace seventv::eventapi

class SeventvPaints
{
public:
    SeventvPaints();

    void addPaint(const QJsonObject &paintJson);
    void assignPaintToUsers(const QString &paintID,
                            std::span<const seventv::eventapi::User> users);
    void clearPaintFromUsers(const QString &paintID,
                             std::span<const seventv::eventapi::User> users);

    /// Returns nullptr for paints the user hid, unless `includeHidden` is set.
    std::shared_ptr<Paint> getPaint(const QString &userName, bool kick,
                                    bool includeHidden = false) const;

    bool isPaintHidden(const QString &paintID) const;
    /// Must be called from the GUI thread.
    void hidePaint(const QString &paintID, const QString &paintName);
    /// Must be called from the GUI thread.
    void unhidePaint(const QString &paintID);

private:
    void updateHiddenPaints();

    mutable std::shared_mutex mutex_;

    std::unordered_map<QString, std::shared_ptr<Paint>> kickPaintMap_;

    std::unordered_map<QString, std::shared_ptr<Paint>> twitchPaintMap_;

    std::unordered_map<QString, std::shared_ptr<Paint>> knownPaints_;

    std::unordered_set<QString> hiddenPaintIDs_;

    pajlada::Signals::SignalHolder signalHolder_;
};

}  // namespace chatterino

#pragma once

#include "messages/Emote.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QString>

#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace chatterino {

struct MoltorinoSupporterBadge {
    QString categoryId;
    EmotePtr emote;
};

class MoltorinoSupporterBadges final : public QObject
{
public:
    explicit MoltorinoSupporterBadges(QObject *parent = nullptr);

    void initialize();
    void refreshNow();
    void refreshPassive();
    void refreshIfNewer(int version);

    std::vector<MoltorinoSupporterBadge> getBadges(const QString &userId) const;

private:
    void refreshInternal(bool force, std::optional<int> minimumVersion);
    void finishRequest();
    void loadCache();
    void saveCache(const QByteArray &payload) const;
    bool applyPayload(const QByteArray &payload, bool fromCache,
                      std::optional<int> minimumVersion = std::nullopt);

    mutable std::shared_mutex mutex_;
    std::unordered_map<QString, std::vector<MoltorinoSupporterBadge>>
        userBadges_;

    int version_ = -1;
    bool initialized_ = false;
    bool requestInFlight_ = false;
    bool pendingRefresh_ = false;
    bool pendingForce_ = false;
    std::optional<int> pendingMinimumVersion_;
    QDateTime lastFetchAttempt_;
};

}  // namespace chatterino

#pragma once

#include <QDateTime>
#include <QString>

#include <functional>
#include <optional>
#include <vector>

namespace chatterino {

constexpr int TWITCH_NAME_HISTORY_LIMIT = 50;

struct TwitchNameHistoryEntry {
    QString login;
    QString leftText;
    QString rightText;
};

struct TwitchNameHistory {
    QString userId;
    QString currentLogin;
    std::vector<TwitchNameHistoryEntry> entries;
    QDateTime fetchedAt;
};

QString normalizeTwitchNameHistoryLogin(const QString &login);

std::optional<TwitchNameHistory> getCachedTwitchNameHistory(
    const QString &userId, const QString &expectedCurrentLogin);

void fetchTwitchNameHistoryByUserId(
    const QString &userId, const QString &requestedLogin,
    std::function<void(TwitchNameHistory)> successCallback,
    std::function<void(const QString &)> failureCallback);

}  // namespace chatterino

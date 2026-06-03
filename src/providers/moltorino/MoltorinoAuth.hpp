#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <optional>
#include <vector>

namespace chatterino {

class TwitchChannel;

struct MoltorinoAuthChannel {
    QString id;
    QString login;
    QString displayName;
};

struct MoltorinoAuthAccount {
    QString userId;
    QString login;
    QString displayName;
    QString token;
    bool valid = false;
    QString lastError;
    QString lastValidatedAt;
    QVector<MoltorinoAuthChannel> moderatedChannels;
};

struct MoltorinoAuthToken {
    QString token;
    QString userId;
    QString login;
    bool legacy = false;

    [[nodiscard]] bool hasToken() const
    {
        return !this->token.trimmed().isEmpty();
    }
};

struct MoltorinoAuthSummary {
    int accountCount = 0;
    int validAccountCount = 0;
    int invalidAccountCount = 0;
    int moderatedChannelCount = 0;
    bool hasLegacyToken = false;
    bool hasOnlyLegacyToken = false;
};

struct MoltorinoAuthRefreshResult {
    int total = 0;
    int valid = 0;
    int invalid = 0;
    int moderatedChannels = 0;
    QStringList errors;
};

namespace MoltorinoAuth {

std::vector<MoltorinoAuthAccount> accounts();
MoltorinoAuthSummary summary();
QString legacyToken();

void addOrUpdateToken(const QString &token,
                      std::function<void(MoltorinoAuthAccount)> successCallback,
                      std::function<void(const QString &)> failureCallback);
void removeAccount(const QString &userId, const QString &token);
void refreshAccounts(std::function<void(MoltorinoAuthRefreshResult)> callback);
void scheduleStartupRefresh();

MoltorinoAuthToken resolveModerationToken(const QString &channelId,
                                          const QString &channelLogin,
                                          QString *errorMessage = nullptr);
MoltorinoAuthToken resolveSavedBroadcasterToken(
    const QString &channelId, const QString &channelLogin,
    QString *errorMessage = nullptr);
MoltorinoAuthToken resolveBroadcasterToken(const QString &channelId,
                                           const QString &channelLogin,
                                           QString *errorMessage = nullptr);
MoltorinoAuthToken resolveSelectedUserToken(QString *errorMessage = nullptr);
MoltorinoAuthToken resolveCurrentUserToken(QString *errorMessage = nullptr);
MoltorinoAuthToken resolveReadToken(QString *errorMessage = nullptr);

QString authRequiredMessage(const QString &action);
QString authExpiredMessage(const QString &action);
QString normalizeAuthError(const QString &action, const QString &error);

}  // namespace MoltorinoAuth

}  // namespace chatterino

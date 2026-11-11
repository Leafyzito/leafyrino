#pragma once

#include <QJsonArray>
#include <QStringList>
#include <QVector>

#include <functional>

class QObject;

namespace chatterino::twitchgifs {

inline constexpr int GIF_PAGE_SIZE = 12;
inline constexpr int MAX_FAVORITES = 120;
inline constexpr int MAX_SEARCH_LENGTH = 50;

struct Gif {
    QString id;
    QString title;
    QString url;
    double aspectRatio = 1.0;
};

struct Config {
    QString apiKey;
    QString rating;
};

struct Page {
    QVector<Gif> gifs;
    int nextOffset = -1;
};

struct SendResult {
    bool sent = false;
    QString error;
    int cooldownSeconds = 0;
    /// Twitch's error code when the rejection is neither a cooldown, a login
    /// failure, nor temporary unavailability. The picker looks up the user's
    /// sub tier before explaining this.
    QString rejectionCode;
};

bool validId(const QString &id);
bool validGif(const Gif &gif);
QStringList favoriteIds(const QString &json);
Page parsePage(const QJsonArray &data, const QString &maximumRating = "pg");

using Failure = std::function<void(QString)>;
void loadConfig(const QString &channelId, const QString &token,
                const QObject *caller, std::function<void(Config)> success,
                Failure failure);
void loadPage(const Config &config, const QString &search,
              const QStringList &ids, int offset, const QObject *caller,
              std::function<void(Page)> success, Failure failure);
void send(const QString &channelId, const QString &token, const Gif &gif,
          const QString &search, const QObject *caller,
          std::function<void(SendResult)> callback);
/// Tier 1 to 3 of the logged-in user's subscription in the channel, or 0.
void loadOwnSubscriptionTier(const QString &channelId, const QString &token,
                             const QObject *caller,
                             std::function<void(int)> success, Failure failure);

}  // namespace chatterino::twitchgifs

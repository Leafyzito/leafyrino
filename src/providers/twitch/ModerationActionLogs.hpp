#pragma once

#include "providers/twitch/api/TwitchGql.hpp"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>

namespace chatterino {

struct ModerationActionLogCounts {
    int bans = 0;
    int timeouts = 0;

    [[nodiscard]] int countedTotal() const;
    [[nodiscard]] int rawTotal() const;
};

struct ModerationActionLogModeratorSummary {
    QString id;
    QString login;
    QString displayName;
    ModerationActionLogCounts counts;
};

struct ModerationActionLogScanRequest {
    QString channelId;
    QString channelLogin;
    QString oauthToken;
    QString moderatorId;
    QString moderatorLogin;
    QDateTime cutoffUtc;
    int maxPages = 400;
    int pageDelayMs = 120;
};

struct ModerationActionLogScanSnapshot {
    QVector<ModerationActionLogModeratorSummary> moderators;
    ModerationActionLogCounts totals;
    int pagesRead = 0;
    int rawActionsSeen = 0;
    bool reachedCutoff = false;
    bool truncated = false;
    bool complete = false;
    bool cancelled = false;
    QString lastCursor;
    QDateTime oldestSeen;
};

QString moderationActionKindText(GqlModerationActionKind kind);
bool moderationActionKindCounts(GqlModerationActionKind kind);

class ModerationActionLogScanner : public QObject
{
public:
    explicit ModerationActionLogScanner(ModerationActionLogScanRequest request,
                                        QObject *parent = nullptr);

    void start();
    void cancel();

    [[nodiscard]] ModerationActionLogScanSnapshot snapshot() const;

    std::function<void(const ModerationActionLogScanSnapshot &)> onProgress;
    std::function<void(const ModerationActionLogScanSnapshot &)> onDone;
    std::function<void(const QString &)> onError;

private:
    struct Accumulator {
        ModerationActionLogModeratorSummary summary;
    };

    void fetchNext();
    void processPage(const GqlModerationActionLogPage &page);
    void processAction(const GqlModerationActionLogEntry &action);
    void emitProgress();
    void finish(bool truncated = false);
    void fail(const QString &message);
    bool matchesModeratorFilter(
        const GqlModerationActionLogEntry &action) const;
    QString moderatorKey(const GqlModerationActionLogEntry &action) const;

    ModerationActionLogScanRequest request_;
    ModerationActionLogScanSnapshot snapshot_;
    QHash<QString, Accumulator> moderators_;
    QString cursor_;
    bool running_ = false;
    bool cancelled_ = false;
};

}  // namespace chatterino

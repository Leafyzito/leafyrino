// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QHash>
#include <QString>
#include <QVector>

#include <optional>

namespace chatterino {

struct RepeatedMessageCheck {
    QString channelID;
    QString userID;
    QString messageID;
    QString message;

    bool historical = false;
    bool channelCanModerate = false;
    bool senderIsModerator = false;
    bool senderIsBroadcaster = false;
    bool senderIsVip = false;
};

class RepeatedMessageDetector final
{
public:
    RepeatedMessageDetector() = default;

    std::optional<int> check(const RepeatedMessageCheck &check);

    void clear();

private:
    struct Entry {
        QString normalizedMessage;
        QString messageID;
        int repeatCount = 1;
        int missesSinceMatch = 0;
        qint64 expiresAt = 0;
    };

    struct SeenMessage {
        QString messageID;
        qint64 expiresAt = 0;
    };

    struct UserCache {
        std::optional<Entry> active;
        std::optional<Entry> candidate;
        QVector<SeenMessage> seenMessages;
        qint64 lastSeenAt = 0;
    };

    using ChannelCache = QHash<QString, UserCache>;

    static QString normalizeMessage(const QString &message);
    static double compare(const QString &first, const QString &second);
    static bool messagesMatch(const QString &first, const QString &second,
                              int sensitivityPercent);
    static int sensitivityToPercent(int sensitivity);
    static Entry makeEntry(const QString &normalizedMessage,
                           const QString &messageID, qint64 now);
    static bool hasSeenMessageID(const UserCache &user,
                                 const QString &messageID);
    static void rememberMessageID(UserCache &user, const QString &messageID,
                                  qint64 now);

    void cleanupUser(UserCache &user, qint64 now);
    void cleanupChannel(ChannelCache &channel, qint64 now);
    void enforceChannelLimit(ChannelCache &channel);

    QHash<QString, ChannelCache> channels_;
    int checksSinceCleanup_ = 0;
};

}  // namespace chatterino

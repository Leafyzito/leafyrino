// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/moltorino/MoltorinoFeatureFlags.hpp"
#include "providers/twitch/TwitchChannel.hpp"

#include <functional>
#include <optional>
#include <QDateTime>
#include <QStringList>
#include <QString>
#include <QVector>

namespace chatterino {

struct PinnedMessage;
class TwitchAccount;

struct CustomAuthValidationResult {
    QString normalizedToken;
    QString userId;
    QString login;
    QString displayName;
};

struct GqlModeratedChannel {
    QString id;
    QString login;
    QString displayName;
};

struct GqlChannelSelfData {
    bool isLeadModerator = false;
};

struct GqlBlockedTerm {
    QString id;
    QString phrase;
    QString expiresAt;
    bool isModEditable = false;
    int hitCount = 0;
};

struct GqlAddBlockedTermResult {
    GqlBlockedTerm term;
    bool wasRemovedFromPermittedList = false;
};

struct GqlUser {
    QString id;
    QString login;
    QString displayName;
};

struct GqlModLogMessage {
    QString id;
    QString text;
    QString sentAt;
};

struct GqlUsercardMessage {
    QString id;
    QString senderId;
    QString senderLogin;
    QString senderDisplayName;
    QString senderColor;
    QString senderBadges;
    QString text;
    QString sentAt;
    QString cursor;
    QString deletedBy;
    bool isDeleted = false;
};

struct GqlUsercardMessagePage {
    QVector<GqlUsercardMessage> messages;
    QString nextCursor;
    bool hasNextPage = false;
};

enum class GqlModerationActionKind {
    Ban,
    Unban,
    Timeout,
    Untimeout,
    Delete,
    Message,
    Other,
};

struct GqlModerationActionLogEntry {
    QString id;
    QString cursor;
    QString category;
    QString icon;
    QString text;
    QDateTime createdAt;
    GqlModerationActionKind kind = GqlModerationActionKind::Other;

    QString moderatorId;
    QString moderatorLogin;
    QString moderatorDisplayName;
    QString targetId;
    QString targetLogin;
    QString targetDisplayName;
};

struct GqlModerationActionLogPage {
    QVector<GqlModerationActionLogEntry> actions;
    QString nextCursor;
    bool hasNextPage = false;
};

struct RaidChannelIDs {
    QString sourceId;
    QString targetId;
    QString targetLogin;
    QString targetDisplayName;
};

struct PredictionTemplate {
    QString title;
    QStringList outcomes;
    int durationSeconds = 120;
};

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
struct GqlChannelPointReward {
    QString id;
    QString title;
    QString prompt;
    QString rewardType;
    QString pricingType;
    QString backgroundColor;
    QString imageUrl;
    int cost = 0;
    bool isAutomatic = false;
    bool isEnabled = false;
    bool isInStock = false;
    bool isUserInputRequired = false;
};

struct GqlChannelPointRewards {
    QString channelId;
    QString channelDisplayName;
    qint64 balance = -1;
    QVector<GqlChannelPointReward> rewards;
};

struct GqlChannelPointEmoteModification {
    QString modifierId;
    QString emoteId;
    QString emoteToken;
};

struct GqlChannelPointEmote {
    QString id;
    QString token;
    QString type;
    QString ownerLogin;
    QString ownerDisplayName;
    QVector<GqlChannelPointEmoteModification> modifications;
};

struct GqlChannelPointEmoteModifier {
    QString id;
    QString title;
};

struct GqlChannelPointRedeemResult {
    qint64 balance = -1;
    QString emoteId;
    QString emoteToken;
};
#endif

class TwitchGql
{
public:
    static void pinMessage(const QString &channelId, const QString &messageId,
                           int durationSeconds, const QString &oauthToken,
                           std::function<void()> successCallback,
                           std::function<void(const QString &)> failureCallback);
    static void unpinMessage(const QString &pinId,
                             const QString &oauthToken,
                             std::function<void()> successCallback,
                             std::function<void(const QString &)> failureCallback);
    static void updatePinnedMessage(const QString &pinId,
                                    std::optional<int> durationSeconds,
                                    const QString &oauthToken,
                                    std::function<void()> successCallback,
                                    std::function<void(const QString &)> failureCallback);
    static void getCurrentPin(const QString &channelId,
                              std::shared_ptr<TwitchAccount> account,
                              std::function<void(std::optional<TwitchChannel::PinnedMessage>)>
                                  successCallback,
                              std::function<void(const QString &)> failureCallback);
    static void getUserByLogin(
        const QString &login, const QString &oauthToken,
        std::function<void(std::optional<GqlUser>)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void followUser(
        const QString &targetId, const QString &oauthToken,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void unfollowUser(
        const QString &targetId, const QString &oauthToken,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getLatestModLogMessageBySender(
        const QString &channelId, const QString &senderId,
        const QString &oauthToken,
        std::function<void(std::optional<GqlModLogMessage>)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getUsercardMessagesBySender(
        const QString &channelId, const QString &senderId,
        const QString &cursor, const QString &oauthToken,
        std::function<void(GqlUsercardMessagePage)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getModerationActionLogs(
        const QString &channelId, const QString &cursor,
        const QString &oauthToken,
        std::function<void(GqlModerationActionLogPage)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getActivePrediction(const QString &channelLogin,
                                    const QString &oauthToken,
                                    std::function<void(std::optional<TwitchChannel::PredictionEvent>)>
                                        successCallback,
                                    std::function<void(const QString &)> failureCallback);
    static void getActivePoll(
        const QString &channelLogin, const QString &oauthToken,
        std::function<void(std::optional<TwitchChannel::PollEvent>)>
            successCallback,
        std::function<void(const QString &)> failureCallback);
    static void makePrediction(const QString &eventID, const QString &outcomeID,
                               int points, const QString &oauthToken,
                               std::function<void()> successCallback,
                               std::function<void(const QString &)> failureCallback);
    static void createPredictionEvent(
        const QString &channelId, const QString &title,
        const QStringList &outcomes, int predictionWindowSeconds,
        const QString &oauthToken,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getPredictionTemplates(
        const QString &channelLogin, const QString &oauthToken,
        std::function<void(QVector<PredictionTemplate>)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void lockPrediction(
        const QString &eventId, const QString &oauthToken,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void cancelPrediction(
        const QString &eventId, const QString &oauthToken,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void resolvePrediction(
        const QString &eventId, const QString &outcomeId,
        const QString &oauthToken,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void createPollEvent(
        const QString &channelId, const QString &title,
        const QStringList &choices, int durationSeconds,
        std::optional<int> pointsPerVote, const QString &oauthToken,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void terminatePoll(const QString &pollId,
                              const QString &currentUserId,
                              const QString &oauthToken,
                              std::function<void()> successCallback,
                              std::function<void(const QString &)> failureCallback);
    static void archivePoll(const QString &pollId, const QString &oauthToken,
                            std::function<void()> successCallback,
                            std::function<void(const QString &)> failureCallback);
    static void addChannelBlockedTerm(
        const QString &channelId, const QString &phrase,
        const QString &oauthToken,
        std::function<void(GqlAddBlockedTermResult)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getChannelBlockedTerms(
        const QString &channelId, const QString &oauthToken,
        std::function<void(QVector<GqlBlockedTerm>)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getChannelSelfData(
        const QString &channelLogin, const QString &oauthToken,
        std::function<void(GqlChannelSelfData)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void deleteChannelBlockedTerm(
        const QString &channelId, const QString &termId,
        const QString &oauthToken, std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void grantVIP(const QString &channelId, const QString &targetLogin,
                         const QString &oauthToken,
                         std::function<void()> successCallback,
                         std::function<void(const QString &)> failureCallback);
    static void revokeVIP(const QString &channelId, const QString &targetLogin,
                          const QString &oauthToken,
                          std::function<void()> successCallback,
                          std::function<void(const QString &)> failureCallback);
    static void modUser(const QString &channelId, const QString &targetLogin,
                        const QString &oauthToken,
                        std::function<void()> successCallback,
                        std::function<void(const QString &)> failureCallback);
    static void unmodUser(const QString &channelId, const QString &targetLogin,
                          const QString &oauthToken,
                          std::function<void()> successCallback,
                          std::function<void(const QString &)> failureCallback);
    static void assignLeadModerator(
        const QString &channelId, const QString &targetUserId,
        const QString &oauthToken, std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void unassignLeadModerator(
        const QString &channelId, const QString &targetUserId,
        const QString &oauthToken, std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void addEditorUser(
        const QString &channelId, const QString &targetLogin,
        const QString &oauthToken, std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void removeEditorUser(
        const QString &channelId, const QString &targetLogin,
        const QString &oauthToken, std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getRaidChannelIDs(
        const QString &sourceLogin, const QString &targetLogin,
        const QString &oauthToken,
        std::function<void(RaidChannelIDs)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void createRaid(const QString &sourceId, const QString &targetId,
                           const QString &oauthToken,
                           std::function<void(const QString &)> successCallback,
                           std::function<void(const QString &)> failureCallback);
    static void sendRaidNow(const QString &sourceId, const QString &oauthToken,
                            std::function<void()> successCallback,
                            std::function<void(const QString &)> failureCallback);
    static void cancelRaidGql(const QString &sourceId, const QString &oauthToken,
                              std::function<void()> successCallback,
                              std::function<void(const QString &)> failureCallback);
    static void voteInPoll(const QString &pollId, const QString &choiceId,
                           const QString &userId, int extraVotes,
                           std::optional<int> pointsPerVote,
                           const QString &oauthToken,
                           std::function<void()> successCallback,
                           std::function<void(const QString &)> failureCallback);
    static void getChannelPoints(const QString &channelLogin,
                                 const QString &oauthToken,
                                 std::function<void(qint64)> successCallback,
                                 std::function<void(const QString &)> failureCallback);
#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
    static void getChannelPointRewards(
        const QString &channelLogin, const QString &oauthToken,
        std::function<void(GqlChannelPointRewards)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void redeemCustomReward(
        const QString &channelId, const GqlChannelPointReward &reward,
        const QString &textInput, const QString &oauthToken,
        std::function<void(GqlChannelPointRedeemResult)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void sendHighlightedChatMessage(
        const QString &channelId, int cost, const QString &message,
        const QString &oauthToken,
        std::function<void(GqlChannelPointRedeemResult)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void sendSubOnlyBypassMessage(
        const QString &channelId, int cost, const QString &message,
        const QString &oauthToken,
        std::function<void(GqlChannelPointRedeemResult)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void unlockRandomSubscriberEmote(
        const QString &channelId, int cost, const QString &oauthToken,
        std::function<void(GqlChannelPointRedeemResult)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void unlockChosenSubscriberEmote(
        const QString &channelId, const QString &emoteId, int cost,
        const QString &oauthToken,
        std::function<void(GqlChannelPointRedeemResult)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void unlockModifiedSubscriberEmote(
        const QString &channelId, const QString &modifiedEmoteId, int cost,
        const QString &oauthToken,
        std::function<void(GqlChannelPointRedeemResult)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getAvailableChannelPointEmotes(
        const QString &channelId, const QString &oauthToken,
        std::function<void(QVector<GqlChannelPointEmote>)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getModifiableChannelPointEmotes(
        const QString &channelLogin, const QString &oauthToken,
        std::function<void(QVector<GqlChannelPointEmote>)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getChannelPointEmoteModifiers(
        const QString &oauthToken,
        std::function<void(QVector<GqlChannelPointEmoteModifier>)> successCallback,
        std::function<void(const QString &)> failureCallback);
#endif
    static void getChatWarningStatus(
        const QString &channelId, const QString &targetUserId,
        const QString &oauthToken,
        std::function<void(std::optional<TwitchChannel::ChatWarning>)>
            successCallback,
        std::function<void(const QString &)> failureCallback);
    static void acknowledgeChatWarning(
        const QString &channelId, const QString &oauthToken,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void validateCustomAuthToken(
        const QString &oauthToken,
        std::function<void(CustomAuthValidationResult)> successCallback,
        std::function<void(const QString &)> failureCallback);
    static void getModeratedChannels(
        const QString &oauthToken,
        std::function<void(QVector<GqlModeratedChannel>)> successCallback,
        std::function<void(const QString &)> failureCallback);
};

}  // namespace chatterino

// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/moltorino/MoltorinoFeatureFlags.hpp"
#include "providers/twitch/TwitchChannel.hpp"

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

namespace chatterino {

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

struct GqlBadge {
    QString id;
    QString setID;
    QString version;
    QString title;
    QString image1x;
    QString image2x;
    QString image4x;
};

struct GqlChatSettingsBadges {
    GqlBadge selectedGlobalBadge;
    QVector<GqlBadge> availableGlobal;

    GqlBadge selectedChannelBadge;
    QVector<GqlBadge> availableChannel;
    QVector<GqlBadge> authorityBadges;
};

#endif

namespace TwitchGql {

void pinMessage(const QString &channelId, const QString &messageId,
                int durationSeconds, const QString &oauthToken,
                std::function<void()> successCallback,
                std::function<void(const QString &)> failureCallback);
void unpinMessage(const QString &pinId, const QString &oauthToken,
                  std::function<void()> successCallback,
                  std::function<void(const QString &)> failureCallback);
void updatePinnedMessage(const QString &pinId,
                         std::optional<int> durationSeconds,
                         const QString &oauthToken,
                         std::function<void()> successCallback,
                         std::function<void(const QString &)> failureCallback);
void getCurrentPin(
    const QString &channelId, std::shared_ptr<TwitchAccount> account,
    std::function<void(std::optional<TwitchChannel::PinnedMessage>)>
        successCallback,
    std::function<void(const QString &)> failureCallback);
void getUserByLogin(const QString &login, const QString &oauthToken,
                    std::function<void(std::optional<GqlUser>)> successCallback,
                    std::function<void(const QString &)> failureCallback);
void followUser(const QString &targetId, const QString &oauthToken,
                std::function<void()> successCallback,
                std::function<void(const QString &)> failureCallback);
void unfollowUser(const QString &targetId, const QString &oauthToken,
                  std::function<void()> successCallback,
                  std::function<void(const QString &)> failureCallback);
void getLatestModLogMessageBySender(
    const QString &channelId, const QString &senderId,
    const QString &oauthToken,
    std::function<void(std::optional<GqlModLogMessage>)> successCallback,
    std::function<void(const QString &)> failureCallback);
void getUsercardMessagesBySender(
    const QString &channelId, const QString &senderId, const QString &cursor,
    const QString &oauthToken,
    std::function<void(GqlUsercardMessagePage)> successCallback,
    std::function<void(const QString &)> failureCallback);
void getModerationActionLogs(
    const QString &channelId, const QString &cursor, const QString &oauthToken,
    std::function<void(GqlModerationActionLogPage)> successCallback,
    std::function<void(const QString &)> failureCallback);
void getActivePrediction(
    const QString &channelLogin, const QString &oauthToken,
    std::function<void(std::optional<TwitchChannel::PredictionEvent>)>
        successCallback,
    std::function<void(const QString &)> failureCallback);
void getActivePoll(const QString &channelLogin, const QString &oauthToken,
                   std::function<void(std::optional<TwitchChannel::PollEvent>)>
                       successCallback,
                   std::function<void(const QString &)> failureCallback);
void makePrediction(const QString &eventID, const QString &outcomeID,
                    int points, const QString &oauthToken,
                    std::function<void()> successCallback,
                    std::function<void(const QString &)> failureCallback);
void createPredictionEvent(
    const QString &channelId, const QString &title, const QStringList &outcomes,
    int predictionWindowSeconds, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback);
void getPredictionTemplates(
    const QString &channelLogin, const QString &oauthToken,
    std::function<void(QVector<PredictionTemplate>)> successCallback,
    std::function<void(const QString &)> failureCallback);
void lockPrediction(const QString &eventId, const QString &oauthToken,
                    std::function<void()> successCallback,
                    std::function<void(const QString &)> failureCallback);
void cancelPrediction(const QString &eventId, const QString &oauthToken,
                      std::function<void()> successCallback,
                      std::function<void(const QString &)> failureCallback);
void resolvePrediction(const QString &eventId, const QString &outcomeId,
                       const QString &oauthToken,
                       std::function<void()> successCallback,
                       std::function<void(const QString &)> failureCallback);
void createPollEvent(const QString &channelId, const QString &title,
                     const QStringList &choices, int durationSeconds,
                     std::optional<int> pointsPerVote,
                     const QString &oauthToken,
                     std::function<void()> successCallback,
                     std::function<void(const QString &)> failureCallback);
void terminatePoll(const QString &pollId, const QString &currentUserId,
                   const QString &oauthToken,
                   std::function<void()> successCallback,
                   std::function<void(const QString &)> failureCallback);
void archivePoll(const QString &pollId, const QString &oauthToken,
                 std::function<void()> successCallback,
                 std::function<void(const QString &)> failureCallback);
void addChannelBlockedTerm(
    const QString &channelId, const QString &phrase, const QString &oauthToken,
    std::function<void(GqlAddBlockedTermResult)> successCallback,
    std::function<void(const QString &)> failureCallback);
void getChannelBlockedTerms(
    const QString &channelId, const QString &oauthToken,
    std::function<void(QVector<GqlBlockedTerm>)> successCallback,
    std::function<void(const QString &)> failureCallback);
void getChannelSelfData(const QString &channelLogin, const QString &oauthToken,
                        std::function<void(GqlChannelSelfData)> successCallback,
                        std::function<void(const QString &)> failureCallback);
void deleteChannelBlockedTerm(
    const QString &channelId, const QString &termId, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback);
void grantVIP(const QString &channelId, const QString &targetLogin,
              const QString &oauthToken, std::function<void()> successCallback,
              std::function<void(const QString &)> failureCallback);
void revokeVIP(const QString &channelId, const QString &targetLogin,
               const QString &oauthToken, std::function<void()> successCallback,
               std::function<void(const QString &)> failureCallback);
void modUser(const QString &channelId, const QString &targetLogin,
             const QString &oauthToken, std::function<void()> successCallback,
             std::function<void(const QString &)> failureCallback);
void unmodUser(const QString &channelId, const QString &targetLogin,
               const QString &oauthToken, std::function<void()> successCallback,
               std::function<void(const QString &)> failureCallback);
void assignLeadModerator(const QString &channelId, const QString &targetUserId,
                         const QString &oauthToken,
                         std::function<void()> successCallback,
                         std::function<void(const QString &)> failureCallback);
void unassignLeadModerator(
    const QString &channelId, const QString &targetUserId,
    const QString &oauthToken, std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback);
void addEditorUser(const QString &channelId, const QString &targetLogin,
                   const QString &oauthToken,
                   std::function<void()> successCallback,
                   std::function<void(const QString &)> failureCallback);
void removeEditorUser(const QString &channelId, const QString &targetLogin,
                      const QString &oauthToken,
                      std::function<void()> successCallback,
                      std::function<void(const QString &)> failureCallback);
void getRaidChannelIDs(const QString &sourceLogin, const QString &targetLogin,
                       const QString &oauthToken,
                       std::function<void(RaidChannelIDs)> successCallback,
                       std::function<void(const QString &)> failureCallback);
void createRaid(const QString &sourceId, const QString &targetId,
                const QString &oauthToken,
                std::function<void(const QString &)> successCallback,
                std::function<void(const QString &)> failureCallback);
void sendRaidNow(const QString &sourceId, const QString &oauthToken,
                 std::function<void()> successCallback,
                 std::function<void(const QString &)> failureCallback);
void cancelRaidGql(const QString &sourceId, const QString &oauthToken,
                   std::function<void()> successCallback,
                   std::function<void(const QString &)> failureCallback);
void voteInPoll(const QString &pollId, const QString &choiceId,
                const QString &userId, int extraVotes,
                std::optional<int> pointsPerVote, const QString &oauthToken,
                std::function<void()> successCallback,
                std::function<void(const QString &)> failureCallback);
void getChannelPoints(const QString &channelLogin, const QString &oauthToken,
                      std::function<void(qint64)> successCallback,
                      std::function<void(const QString &)> failureCallback);
#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
void getChannelPointRewards(
    const QString &channelLogin, const QString &oauthToken,
    std::function<void(GqlChannelPointRewards)> successCallback,
    std::function<void(const QString &)> failureCallback);
void redeemCustomReward(
    const QString &channelId, const GqlChannelPointReward &reward,
    const QString &textInput, const QString &oauthToken,
    std::function<void(GqlChannelPointRedeemResult)> successCallback,
    std::function<void(const QString &)> failureCallback);
void sendHighlightedChatMessage(
    const QString &channelId, int cost, const QString &message,
    const QString &oauthToken,
    std::function<void(GqlChannelPointRedeemResult)> successCallback,
    std::function<void(const QString &)> failureCallback);
void sendSubOnlyBypassMessage(
    const QString &channelId, int cost, const QString &message,
    const QString &oauthToken,
    std::function<void(GqlChannelPointRedeemResult)> successCallback,
    std::function<void(const QString &)> failureCallback);
void unlockRandomSubscriberEmote(
    const QString &channelId, int cost, const QString &oauthToken,
    std::function<void(GqlChannelPointRedeemResult)> successCallback,
    std::function<void(const QString &)> failureCallback);
void unlockChosenSubscriberEmote(
    const QString &channelId, const QString &emoteId, int cost,
    const QString &oauthToken,
    std::function<void(GqlChannelPointRedeemResult)> successCallback,
    std::function<void(const QString &)> failureCallback);
void unlockModifiedSubscriberEmote(
    const QString &channelId, const QString &modifiedEmoteId, int cost,
    const QString &oauthToken,
    std::function<void(GqlChannelPointRedeemResult)> successCallback,
    std::function<void(const QString &)> failureCallback);
void getAvailableChannelPointEmotes(
    const QString &channelId, const QString &oauthToken,
    std::function<void(QVector<GqlChannelPointEmote>)> successCallback,
    std::function<void(const QString &)> failureCallback);
void getModifiableChannelPointEmotes(
    const QString &channelLogin, const QString &oauthToken,
    std::function<void(QVector<GqlChannelPointEmote>)> successCallback,
    std::function<void(const QString &)> failureCallback);
void getChannelPointEmoteModifiers(
    const QString &oauthToken,
    std::function<void(QVector<GqlChannelPointEmoteModifier>)> successCallback,
    std::function<void(const QString &)> failureCallback);
#endif
void getChatWarningStatus(
    const QString &channelId, const QString &targetUserId,
    const QString &oauthToken,
    std::function<void(std::optional<TwitchChannel::ChatWarning>)>
        successCallback,
    std::function<void(const QString &)> failureCallback);
void acknowledgeChatWarning(
    const QString &channelId, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback);
void validateCustomAuthToken(
    const QString &oauthToken,
    std::function<void(CustomAuthValidationResult)> successCallback,
    std::function<void(const QString &)> failureCallback);
void getModeratedChannels(
    const QString &oauthToken,
    std::function<void(QVector<GqlModeratedChannel>)> successCallback,
    std::function<void(const QString &)> failureCallback);
void getChatSettingsBadges(
    const QString &channelLogin, const QString &oauthToken,
    std::function<void(GqlChatSettingsBadges)> successCallback,
    std::function<void(const QString &)> failureCallback);
void selectGlobalBadge(
    const QString &badgeSetID, const QString &badgeSetVersion,
    const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback);
void selectChannelBadge(
    const QString &badgeSetID, const QString &badgeSetVersion,
    const QString &channelID, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback);

}  // namespace TwitchGql

}  // namespace chatterino

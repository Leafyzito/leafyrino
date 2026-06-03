// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/moltorino/MoltorinoFeatureFlags.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchIrc.hpp"

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QObject;

namespace chatterino {

class TwitchAccount;

/**
 * Undocumented Twitch GraphQL (https://gql.twitch.tv/gql). Used where Helix cannot
 * satisfy the product need (e.g. viewing another channel's predictions). May require
 * Client-Integrity (JWT from POST /integrity), can change without notice, and may have
 * ToS implications — see implementation comments.
 */
namespace TwitchGql {

/// Active poll shown to viewers (`User.viewablePoll` / `channel.owner.viewablePoll` on gql.twitch.tv).
struct ViewablePollChoice {
    QString id;
    QString title;
    int votesTotal{};
    int totalVoters{};
};

struct ViewablePoll {
    QString id;
    QString title;
    QString status;
    qint64 remainingDurationMs{};
    int durationSeconds{};
    QString startedAt;
    QString endedAt;
    int totalVoters{};
    std::vector<ViewablePollChoice> choices;
    /// Choice IDs the signed-in viewer selected (from `self.voter.choices`), if any.
    std::vector<QString> viewerVotedChoiceIds;
    /// For terminal polls: choice with highest `votes.total` (first wins on ties). `Poll` has no `winningChoice` field in GQL.
    QString winningChoiceId;
};

/// `viewablePoll.status` is ACTIVE while the poll runs.
inline bool pollStatusIsActive(const QString &status)
{
    return status.compare(QStringLiteral("ACTIVE"), Qt::CaseInsensitive) == 0;
}

/// Terminal poll statuses seen from Twitch GQL (may grow; unknown statuses are dropped in the parser).
inline bool pollStatusIsTerminal(const QString &status)
{
    return status.compare(QStringLiteral("COMPLETED"), Qt::CaseInsensitive) ==
               0 ||
           status.compare(QStringLiteral("TERMINATED"), Qt::CaseInsensitive) ==
               0 ||
           status.compare(QStringLiteral("ARCHIVED"), Qt::CaseInsensitive) ==
               0 ||
           status.compare(QStringLiteral("ENDED"), Qt::CaseInsensitive) == 0 ||
           status.compare(QStringLiteral("CLOSED"), Qt::CaseInsensitive) == 0;
}

struct PinnedChatMessage {
    QString id;
    QString sentAt;  // ISO timestamp from GQL
    QString senderDisplayName;
    /// Lowercase login when returned by GQL (`sender.login`).
    QString senderLogin;
    /// Numeric Twitch user id when returned by GQL (`sender.id`).
    QString senderId;
    /// Hex color e.g. `#FF0000` when returned (`sender.chatColor`).
    QString senderChatColor;
    QString text;
    std::vector<TwitchEmoteOccurrence> twitchEmotes;
};

/// Fetches active/locked prediction for the channel; maps GQL into HelixPrediction for UI.
/// \a clientId is ignored: gql.twitch.tv rejects most Helix app Client-IDs with HTTP 400;
/// requests use Twitch's public web Client-ID; \a oauthToken is still sent as Bearer.
void fetchPredictionsForChannel(
    const QString &channelId, const QString &channelLogin,
    const QString &clientId, const QString &oauthToken, const QObject *caller,
    std::function<void(std::optional<HelixPrediction>)> onSuccess,
    std::function<void(QString)> onError);

/// Undocumented GQL: channel poll for viewers (`User.viewablePoll` / `channel.owner.viewablePoll`).
/// Returns ACTIVE polls and terminal results while Twitch still exposes them. Uses the same
/// gql.twitch.tv + Client-Integrity path as predictions. Request sends the full `query` document
/// plus `extensions.persistedQuery.sha256Hash` = SHA256(UTF-8 query) — Twitch executes that
/// document; the hash alone is not registered on Twitch's APQ map. If Twitch changes fields,
/// update the query string (see https://kawcco.com/twitch-graphql-api/poll.doc.html) and DevTools.
/// Casting votes uses undocumented `VoteInPoll` and may break without notice.
void fetchViewablePollForChannel(
    const QString &channelId, const QString &channelLogin,
    const QString &clientId, const QString &oauthToken, const QObject *caller,
    std::function<void(std::optional<ViewablePoll>)> onSuccess,
    std::function<void(QString)> onError);

/// Persisted query GetPinnedChat: fetches the current moderator-pinned chat message.
/// Returns std::nullopt when no pinned message is present.
void fetchPinnedChatMessage(
    const QString &channelId, int count, const QString &oauthToken,
    const QString &gqlClientId, const QObject *caller,
    std::function<void(std::optional<PinnedChatMessage>)> onSuccess,
    std::function<void(QString)> onError);

/// Persisted query ChannelPointsContext: viewer's channel points balance for @a channelLogin.
/// \a gqlClientId is the Twitch OAuth Client ID stored with the account (e.g. TV device-flow id);
/// when it matches Twitch's TV app id, requests use the same headers as channel-points-miner
/// (OAuth scheme, Client-Id, X-Device-Id, etc.). Otherwise the public web Client-ID + Bearer path
/// is used.
void fetchChannelPointsBalance(const QString &channelLogin,
                               const QString &oauthToken,
                               const QString &gqlClientId,
                               const QObject *caller,
                               std::function<void(int)> onSuccess,
                               std::function<void(QString)> onError);

/// Persisted query MakePrediction (same hash as channel-points-miner). Places a
/// channel-points bet on an ACTIVE prediction; \a gqlClientId selects TV vs web headers.
void makePrediction(const QString &eventId, const QString &outcomeId,
                    int points, const QString &oauthToken,
                    const QString &gqlClientId, const QObject *caller,
                    std::function<void()> onSuccess,
                    std::function<void(QString)> onError);

/// Undocumented `VoteInPoll` mutation (same transport as `makePrediction`). Free single-vote
/// path only; channel-points multi-vote may require extra fields not implemented here.
void voteInPoll(const QString &pollId, const QString &choiceId,
                const QString &userId, const QString &oauthToken,
                const QString &gqlClientId, const QObject *caller,
                std::function<void()> onSuccess,
                std::function<void(QString)> onError);

}  // namespace TwitchGql

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
void getCurrentPin(const QString &channelId,
                   std::shared_ptr<TwitchAccount> account,
                   std::function<void(std::optional<TwitchChannel::PinnedMessage>)>
                       successCallback,
                   std::function<void(const QString &)> failureCallback);
void getUserByLogin(
        const QString &login, const QString &oauthToken,
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
void getActivePoll(
    const QString &channelLogin, const QString &oauthToken,
    std::function<void(std::optional<TwitchChannel::PollEvent>)> successCallback,
    std::function<void(const QString &)> failureCallback);
void makePrediction(const QString &eventID, const QString &outcomeID,
                    int points, const QString &oauthToken,
                    std::function<void()> successCallback,
                    std::function<void(const QString &)> failureCallback);
void createPredictionEvent(
    const QString &channelId, const QString &title,
    const QStringList &outcomes, int predictionWindowSeconds,
    const QString &oauthToken, std::function<void()> successCallback,
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
void createPollEvent(
    const QString &channelId, const QString &title,
    const QStringList &choices, int durationSeconds,
    std::optional<int> pointsPerVote, const QString &oauthToken,
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
void deleteChannelBlockedTerm(const QString &channelId, const QString &termId,
                              const QString &oauthToken,
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
void unassignLeadModerator(const QString &channelId,
                           const QString &targetUserId,
                           const QString &oauthToken,
                           std::function<void()> successCallback,
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
void acknowledgeChatWarning(const QString &channelId, const QString &oauthToken,
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

}  // namespace TwitchGql

}  // namespace chatterino

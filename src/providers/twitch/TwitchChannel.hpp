// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"
#include "common/Atomic.hpp"
#include "common/Channel.hpp"
#include "common/ChannelChatters.hpp"
#include "common/Common.hpp"
#include "common/UniqueAccess.hpp"
#include "providers/ffz/FfzBadges.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/eventsub/SubscriptionHandle.hpp"
#include "providers/twitch/TwitchEmotes.hpp"
#include "util/QStringHash.hpp"
#include "util/ThreadGuard.hpp"

#include <boost/circular_buffer/space_optimized.hpp>
#include <boost/signals2.hpp>
#include <IrcMessage>
#include <pajlada/signals/signalholder.hpp>
#include <QColor>
#include <QDateTime>
#include <QElapsedTimer>
#include <QRegularExpression>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

class TestIrcMessageHandlerP;
class TestEventSubMessagesP;

namespace chatterino {

class TwitchChannelTestAccess;

enum class HighlightState;

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;
struct LiveUpdateEmote;
class EmoteMap;

class TwitchBadges;
class FfzEmotes;
class BttvEmotes;
struct BttvLiveUpdateEmoteUpdateAddMessage;
struct BttvLiveUpdateEmoteRemoveMessage;

class SeventvEmotes;
namespace seventv::eventapi {
struct EmoteAddDispatch;
struct EmoteUpdateDispatch;
struct EmoteRemoveDispatch;
struct UserConnectionUpdateDispatch;
}  // namespace seventv::eventapi

struct ChannelPointReward;
class MessageThread;
struct CheerEmoteSet;
struct HelixStream;
struct HelixCheermoteSet;
struct HelixGlobalBadges;
using HelixChannelBadges = HelixGlobalBadges;
struct HelixPinnedChatMessage;

class TwitchIrcServer;
class TwitchAccount;

const int MAX_QUEUED_REDEMPTIONS = 16;
const int MAX_RECENT_CHANNEL_POINT_REDEMPTIONS = 1024;

namespace detail {

/// isUnknownCommand checks if the given text contains a command that should not be forwarded to Twitch
///
/// "/ hello" should be allowed
/// ". hello" should be allowed
/// "/me hello" should be allowed
/// ".me hello" should be allowed
/// "/mebadcommand hello" should NOT be allowed
/// ".mebadcommand hello" should NOT be allowed
/// "/badcommand hello" should NOT be allowed
/// "/badcommand hello" should NOT be allowed
/// ".@badcommand hello" should NOT be allowed
/// ".@badcommand hello" should NOT be allowed
bool isUnknownCommand(const QString &text);

}  // namespace detail

class TwitchChannel final : public Channel, public ChannelChatters
{
public:
    /**
     * @lua@class StreamStatus
     */
    struct StreamStatus {
        /**
         * @lua@field live boolean
         */
        bool live = false;
        bool rerun = false;
        /**
         * @lua@field viewer_count number
         */
        unsigned viewerCount = 0;
        /**
         * @lua@field title string Stream title or last stream title
         */
        QString title;
        /**
         * @lua@field game_name string
         */
        QString game;
        /**
         * @lua@field game_id string
         */
        QString gameId;
        QString uptime;
        /**
         * @lua@field uptime number Seconds since the stream started.
         */
        int uptimeSeconds = 0;
        QString streamType;
        QString streamId;
    };

    /**
     * @lua@class RoomModes
     */
    struct RoomModes {
        /**
         * @lua@field subscriber_only boolean
         */
        bool submode = false;
        /**
         * @lua@field unique_chat boolean You might know this as r9kbeta or robot9000.
         */
        bool r9k = false;
        /**
         * @lua@field emotes_only boolean Whether or not text is allowed in messages. Note that "emotes" here only means Twitch emotes, not Unicode emoji, nor 3rd party text-based emotes
         */
        bool emoteOnly = false;

        /**
         * @lua@field follower_only number? Time in minutes you need to follow to chat or nil.
         */
        /**
         * @brief Number of minutes required for users to be followed before typing in chat
         *
         * Special cases:
         * -1 = follower mode off
         *  0 = follower mode on, no time requirement
         */
        int followerOnly = -1;

        /**
         * @lua@field slow_mode number? Time in seconds you need to wait before sending messages or nil.
         */
        /**
         * @brief Number of seconds required to wait before typing emotes
         *
         * 0 = slow mode off
         */
        int slowMode = 0;
    };

    struct PinnedMessage {
        QString pinId;
        QString messageId;
        QString text;
        QString authorId;
        QString authorName;
        QString authorLogin;
        QString authorColor;
        QString authorBadges;
        QString pinnerName;
        QString pinnerLogin;
        std::optional<QDateTime> endsAt;
        std::optional<QDateTime> pinnedAt;
    };

    struct PredictionOutcome {
        QString id;
        QString title;
        QString color;  // "BLUE", "PINK", "GREEN"
        qlonglong totalPoints = 0;
        int totalUsers = 0;
        qlonglong topPoints = 0;
        QString topPredictorName;
    };

    struct PredictionEvent {
        QString id;
        QString title;
        QString status;  // "ACTIVE", "LOCKED", "RESOLVED", "CANCELED"
        std::vector<PredictionOutcome> outcomes;  // 2-10
        int predictionWindowSeconds = 0;
        QDateTime createdAt;
        std::optional<QDateTime> lockedAt;
        QString winningOutcomeId;
        QString createdByName;
        QString lockedByName;
        QString endedByName;  // mod who resolved/canceled
        QString selfOutcomeId;
        int selfPoints = 0;
    };

    struct PollChoice {
        QString id;
        QString title;
        int totalVotes = 0;
        int freeVotes = 0;
        int channelPointsVotes = 0;
        int totalVoters = 0;
        int topChannelPointsContribution = 0;
        QString topChannelPointsContributorName;
    };

    struct PollSelfVote {
        QString choiceId;
        int freeVotes = 0;
        int channelPointsVotes = 0;
    };

    struct PollEvent {
        QString id;
        QString title;
        QString status;  // "ACTIVE", "COMPLETED", "TERMINATED", "ARCHIVED"
        std::vector<PollChoice> choices;  // 2-5
        int durationSeconds = 0;
        int remainingDurationMilliseconds = 0;
        int totalVotes = 0;
        QDateTime createdAt;
        std::optional<QDateTime> endsAt;
        QString createdByName;
        QString endedByName;  // mod who terminated/archived
        QString currentUserId;
        bool channelPointsVotingEnabled = false;
        int pointsPerVote = 0;
        std::vector<PollSelfVote> selfVotes;
    };

    struct ChatWarning {
        QString id;
        QString channelId;
        QString reason;
        QDateTime createdAt;
    };

    struct RaidEvent {
        QString id;
        QString sourceId;
        QString targetId;
        QString targetLogin;
        QString targetDisplayName;
        QString targetProfileImage;
        int viewerCount = 0;
        int forceRaidNowSeconds = 90;
        QDateTime raidCreatedAt;
        QDateTime receivedAt;
    };

    explicit TwitchChannel(const QString &channelName, bool anonymous = false);
    ~TwitchChannel() override;

    TwitchChannel(const TwitchChannel &) = delete;
    TwitchChannel(TwitchChannel &&) = delete;
    TwitchChannel &operator=(const TwitchChannel &) = delete;
    TwitchChannel &operator=(TwitchChannel &&) = delete;

    std::shared_ptr<TwitchChannel> sharedFromThis();
    std::weak_ptr<TwitchChannel> weakFromThis();

    void initialize();

    // Channel methods
    bool isEmpty() const override;
    bool canSendMessage() const override;
    bool isAnonymous() const;
    void sendMessage(const QString &message) override;
    bool sendMessageViaIrc(const QString &message, int duplicateNonce = 0);
    bool sendSpamMessageViaHelix(
        const QString &message, int duplicateNonce,
        std::function<void(bool sent, QString error)> callback);
    void sendBotMessage(const QString &message);
    void sendReply(const QString &message, const QString &replyId);
    bool isMod() const override;
    bool isLeadMod() const;
    bool isVip() const;
    bool isStaff() const;
    bool isFollowing() const;
    bool isFollowingStatusKnown() const;
    bool isBroadcaster() const override;
    bool hasHighRateLimit() const override;
    bool canReconnect() const override;
    void reconnect() override;
    QString getCurrentStreamID() const override;
    void createClip(const QString &title, std::optional<int> duration);

    /// Delete the message with the specified ID as a moderator.
    ///
    /// If the ID is empty, all messages will be deleted, effectively clearing
    /// the chat.
    void deleteMessagesAs(const QString &messageID, TwitchAccount *moderator);

    // Data
    const QString &subscriptionUrl();
    const QString &channelUrl();
    const QString &popoutPlayerUrl();
    int chatterCount() const;
    bool isLive() const override;
    bool isRerun() const override;
    QString roomId() const;
    SharedAccessGuard<const RoomModes> accessRoomModes() const;
    SharedAccessGuard<const StreamStatus> accessStreamStatus() const;
    SharedAccessGuard<const std::optional<PinnedMessage>> accessPinnedMessage()
        const;
    void setPinnedMessage(std::optional<PinnedMessage> pin);
    void refreshPinnedMessage();
    void pinMessage(const QString &messageId, int durationSeconds = 1200);
    void unpinMessage();
    void keepPinned();
    /**
     * Clears the Helix-backed pinned message for this channel immediately
     * (e.g. on unpin PubSub event).
     */
    void clearPinnedMessage();
    /// Returns the currently pinned Helix message, or null if none is pinned.
    const HelixPinnedChatMessage *getPinnedMessage() const;
    /**
     * Unpin the currently pinned message via Helix. Only valid for moderators.
     */
    void unpinCurrentMessage();
    void refreshFollowingStatus(bool force = false);
    void setFollowingStatus(bool following,
                            std::optional<QDateTime> followedAt = std::nullopt);
    void refreshActivePrediction();
    void refreshLeadModStatus(bool force = false);
    void refreshChannelPoints();
    void refreshChatters();
    void refreshChannelPointsIfStale(bool force = false);
    void handlePinnedChatUpdate(const QJsonObject &data);
    void tryEmitPinnedChatPinSystemMessage(const QJsonObject &innerData);

    // Predictions & Points
    SharedAccessGuard<const std::optional<PredictionEvent>> accessPrediction()
        const;
    void setActivePrediction(std::optional<PredictionEvent> prediction);
    void handlePredictionUpdate(const QJsonObject &data);
    void handleUserPointsUpdate(const QJsonObject &payload);
    void refreshPrediction(bool force = false);
    SharedAccessGuard<const std::optional<PollEvent>> accessPoll() const;
    void setActivePoll(std::optional<PollEvent> poll);
    void handlePollUpdate(const QJsonObject &payload);
    void refreshActivePoll();
    void refreshPollIfStale(bool force = false);
    void tryEmitPollCreatedSystemMessage(const PollEvent &poll);
    qint64 channelPointBalance() const;
    bool isChannelPointsFetchInFlight() const;
    bool shouldShowChannelPoints() const;
    void setChannelPointBalance(qint64 balance);
    void handleChatWarningPubSub(const QJsonObject &payload);
    void handleChatWarningNotice();
    void refreshChatWarningIfStale(bool force = false,
                                   bool notifyOnError = false);
    void acknowledgeChatWarning();
    void showPendingChatWarningIfVisible();
    SharedAccessGuard<const std::optional<RaidEvent>> accessRaid() const;
    void setActiveRaid(std::optional<RaidEvent> raid);
    void clearActiveRaid();
    void handleRaidUpdate(const QJsonObject &payload);

    /**
     * Records that the channel is no longer joined.
     */
    void markDisconnected();

    /**
     * Records that the channel's read connection is healthy.
     */
    void markConnected();

    // Emotes
    std::optional<EmotePtr> twitchEmote(const EmoteName &name) const;
    std::optional<EmotePtr> bttvEmote(const EmoteName &name) const;
    std::optional<EmotePtr> ffzEmote(const EmoteName &name) const;
    std::optional<EmotePtr> seventvEmote(const EmoteName &name) const;

    std::shared_ptr<const EmoteMap> localTwitchEmotes() const;
    std::shared_ptr<const EmoteMap> bttvEmotes() const;
    std::shared_ptr<const EmoteMap> ffzEmotes() const;
    std::shared_ptr<const EmoteMap> seventvEmotes() const;

    void refreshTwitchChannelEmotes(bool manualRefresh);
    void refreshBTTVChannelEmotes(bool manualRefresh);
    void refreshFFZChannelEmotes(bool manualRefresh);
    void refreshSevenTVChannelEmotes(bool manualRefresh);
    void refreshBadgesProviders();

    void setBttvEmotes(std::shared_ptr<const EmoteMap> &&map);
    void setFfzEmotes(std::shared_ptr<const EmoteMap> &&map);
    void setSeventvEmotes(std::shared_ptr<const EmoteMap> &&map);

    const QString &seventvUserID() const;
    const QString &seventvEmoteSetID() const;

    /** Adds a BTTV channel emote to this channel. */
    void addBttvEmote(const BttvLiveUpdateEmoteUpdateAddMessage &message);
    /** Updates a BTTV channel emote in this channel. */
    void updateBttvEmote(const BttvLiveUpdateEmoteUpdateAddMessage &message);
    /** Removes a BTTV channel emote from this channel. */
    void removeBttvEmote(const BttvLiveUpdateEmoteRemoveMessage &message);

    /** Adds a 7TV channel emote to this channel. */
    void addSeventvEmote(const seventv::eventapi::EmoteAddDispatch &dispatch);
    /** Updates a 7TV channel emote's name in this channel */
    void updateSeventvEmote(
        const seventv::eventapi::EmoteUpdateDispatch &dispatch);
    /** Removes a 7TV channel emote from this channel */
    void removeSeventvEmote(
        const seventv::eventapi::EmoteRemoveDispatch &dispatch);
    /** Updates the current 7TV user. Currently, only the emote-set is updated. */
    void updateSeventvUser(
        const seventv::eventapi::UserConnectionUpdateDispatch &dispatch);

    // Update the channel's 7TV information (the channel's 7TV user ID and emote set ID)
    void updateSeventvData(const QString &newUserID,
                           const QString &newEmoteSetID);

    // Badges
    std::optional<EmotePtr> ffzCustomModBadge() const;
    std::optional<EmotePtr> ffzCustomVipBadge() const;
    std::optional<EmotePtr> twitchBadge(const QString &set,
                                        const QString &version) const;
    /**
     * Returns a list of channel-specific FrankerFaceZ badges for the given user
     */
    std::vector<FfzBadges::Badge> ffzChannelBadges(const QString &userID) const;
    void setFfzChannelBadges(FfzChannelBadgeMap map);
    void setFfzCustomModBadge(std::optional<EmotePtr> badge);
    void setFfzCustomVipBadge(std::optional<EmotePtr> badge);

    void addTwitchBadgeSets(const HelixChannelBadges &channelBadges);

    // Cheers
    std::optional<CheerEmote> cheerEmote(const QString &string) const;
    void setCheerEmoteSets(const std::vector<HelixCheermoteSet> &cheermoteSets);

    // Replies
    /**
     * Stores the given thread in this channel.
     *
     * Note: This method not take ownership of the MessageThread; this
     * TwitchChannel instance will store a weak_ptr to the thread.
     */
    void addReplyThread(const std::shared_ptr<MessageThread> &thread);
    const std::unordered_map<QString, std::weak_ptr<MessageThread>> &threads()
        const;

    /**
     * Get the thread for the given message
     * If no thread can be found for the message, create one
     */
    std::shared_ptr<MessageThread> getOrCreateThread(const MessagePtr &message);

    /// Fired when a message is supposed to be sent by the user in this channel.
    ///
    /// This should only be handled by one component.
    ///
    /// Arguments:
    /// - `messageText`: The text to be sent.
    /// - `wasSent`: A return channel for whether the message was sent or not.
    pajlada::Signals::Signal<const QString &, bool &> sendMessageSignal;

    /// Fired when a reply to a message is supposed to be sent by the user in
    /// this channel.
    ///
    /// This should only be handled by one component.
    ///
    /// Arguments:
    /// - `messageText`: The text to be sent.
    /// - `replyToMessageID`: The ID of the replied-to message.
    /// - `wasSent`: A return channel for whether the message was sent or not.
    pajlada::Signals::Signal<const QString &, const QString &, bool &>
        sendReplySignal;

    /**
     * This signal fires when the local user has joined the channel
     **/
    pajlada::Signals::NoArgSignal joined;

    // Only TwitchChannel may invoke this signal
    pajlada::Signals::NoArgSignal userStateChanged;

    /**
     * This signal fires whenever the stream status is changed
     *
     * This includes when the stream goes from offline to online,
     * or the viewer count changes, or the title has been updated
     **/
    pajlada::Signals::NoArgSignal streamStatusChanged;

    pajlada::Signals::NoArgSignal roomModesChanged;
    pajlada::Signals::NoArgSignal pinnedMessageChanged;
    pajlada::Signals::NoArgSignal predictionChanged;
    pajlada::Signals::NoArgSignal pollChanged;
    pajlada::Signals::NoArgSignal channelPointsChanged;
    pajlada::Signals::NoArgSignal followingStatusChanged;
    pajlada::Signals::NoArgSignal raidChanged;

    const QString &lastChannelPointsError() const
    {
        return this->lastChannelPointsError_;
    }

    /// Fires when IRC room-id is first set or changes for this channel.
    pajlada::Signals::NoArgSignal roomIdAssigned;

    pajlada::Signals::NoArgSignal destroyed;

    pajlada::Signals::Signal<const QString &> sendWaitUpdate;

    pajlada::Signals::Signal<const std::vector<HelixMinimalUser> &>
        sharedChatStatusChanged;

    // Channel point rewards
    void addQueuedRedemption(const QString &rewardId,
                             const QString &originalContent,
                             Communi::IrcMessage *message);
    /**
     * A rich & hydrated redemption from PubSub has arrived, add it to the channel.
     * This will look at queued up partial messages, and if one is found it will add the queued up partial messages fully hydrated.
     **/
    void addChannelPointReward(const ChannelPointReward &reward);
    /// Adds @a reward to the known rewards
    ///
    /// Unlike in #addChannelPointReward(), no message will be sent.
    void addKnownChannelPointReward(const ChannelPointReward &reward);
    bool isChannelPointRewardKnown(const QString &rewardId);
    std::optional<ChannelPointReward> channelPointReward(
        const QString &rewardId) const;
    bool markChannelPointRedemptionSeen(const QString &key);

    // Live status
    void updateStreamStatus(const std::optional<HelixStream> &helixStream,
                            bool isInitialUpdate);
    void updateStreamTitle(const QString &title);

    /**
     * Returns the display name of the user
     *
     * If the display name contained chinese, japenese, or korean characters, the user's login name is returned instead
     **/
    const QString &getDisplayName() const override;
    void updateDisplayName(const QString &displayName);

    /**
     * Sync the text of the send wait timer to the actual time remaining.
     */
    void syncSendWaitTimer();
    /**
     * Set the send wait timer to given number of seconds
     *
     * When a channel is in slow mode or the user is timed out, calling
     * this function sets the timer to show how long the user will need
     * to wait before they can send again.
     */
    void setSendWait(int seconds);

    bool isLoadingRecentMessages() const;

    const std::vector<HelixMinimalUser> &getSharedChatSessionParticipants()
        const;

private:
    struct NameOptions {
        // displayName is the non-CJK-display name for this user
        // This will always be the same as their `name_`, but potentially with different casing
        QString displayName;

        // localizedName is their display name that *may* contain CJK characters
        // If the display name does not contain any CJK characters, this will be
        // the same as `displayName`
        QString localizedName;

        // actualDisplayName is the raw display name string received from Twitch
        QString actualDisplayName;
    } nameOptions;

    struct QueuedRedemption {
        QString rewardID;
        QString originalContent;
        QObjectPtr<Communi::IrcMessage> message;
    };

    bool anonymous_ = false;

    void refreshPubSub();
    void refreshBadges();
    void refreshCheerEmotes();
    void loadRecentMessages();
    void loadRecentMessagesReconnect();
    void cleanUpReplyThreads();
    void showLoginMessage();
    void showAnonymousReadOnlyMessage();

    /// roomIdChanged is called whenever this channel's ID has been changed
    /// This should only happen once per channel, whenever the ID goes from unset to set
    void roomIdChanged();

    void probeSharedChatSession();
    void refreshSharedChatSessionState();

    /** Joins (subscribes to) a Twitch channel for updates on BTTV. */
    void joinBttvChannel() const;

    void updateBttvActivity();

    /**
     * Indicates an activity to 7TV in this channel for this user.
     * This is done at most once every 60s.
     */
    void updateSevenTVActivity();
    void listenSevenTVCosmetics() const;

    /**
     * @brief Sets the live status of this Twitch channel
     *
     * Returns true if the live status changed with this call
     **/
    bool setLive(bool newLiveStatus);
    void setMod(bool value);
    void setLeadMod(bool value, bool known = true);
    void setVIP(bool value);
    void setStaff(bool value);
    void setRoomId(const QString &id);
    void setRoomModes(const RoomModes &newRoomModes);
    void setDisplayName(const QString &name);
    void setLocalizedName(const QString &name);

    void onLiveStatusChanged(bool isLive, bool isInitialUpdate);

    /**
     * Returns the localized name of the user
     **/
    const QString &getLocalizedName() const override;

    QString prepareMessage(const QString &message,
                           int duplicateNonce = 0) const;
    void setActiveChatWarning(std::optional<ChatWarning> warning,
                              bool showIfVisible);
    void clearChatWarning();
    void showChatWarningMessage(const ChatWarning &warning);
    void showChatWarningAuthFallback();

    /**
     * Either adds a message mentioning the updated emotes
     * or replaces an existing message. For criteria on existing messages,
     * see `tryReplaceLastLiveUpdateAddOrRemove`.
     *
     * @param isEmoteAdd true if the emote was added, false if it was removed.
     * @param platform The platform the emote was updated on ("7TV", "BTTV", "FFZ")
     * @param actor The actor performing the update (possibly empty)
     * @param emote The emote that was added or removed
     */
    void addOrReplaceLiveUpdatesAddRemove(bool isEmoteAdd,
                                          const QString &platform,
                                          const QString &actor,
                                          const LiveUpdateEmote &emote);

    /**
     * Tries to replace the last emote update message.
     *
     * A last message is valid if:
     *  * The actors match
     *  * The operations match
     *  * The platform matches
     *  * The last message isn't older than 5s
     *
     * @param op The emote operation (LiveUpdatesAdd or LiveUpdatesRemove)
     * @param platform The emote platform  ("7TV", "BTTV", "FFZ")
     * @param actor The actor performing the action (possibly empty)
     * @param emote The updated emote
     * @return true, if the last message was replaced
     */
    bool tryReplaceLastLiveUpdateAddOrRemove(MessageFlag op,
                                             const QString &platform,
                                             const QString &actor,
                                             const LiveUpdateEmote &emote);

    // Data
    const QString subscriptionUrl_;
    const QString channelUrl_;
    const QString popoutPlayerUrl_;
    int chatterCount_{};
    std::atomic<bool> chatterFetchInFlight_{false};
    QDateTime lastChatterRefreshAt_;

    UniqueAccess<StreamStatus> streamStatus_;
    UniqueAccess<RoomModes> roomModes;
    UniqueAccess<std::optional<PinnedMessage>> currentPin_;
    std::atomic<int> pinnedMessageRefreshFailures_{0};
    UniqueAccess<std::optional<PredictionEvent>> activePrediction_;
    std::atomic<bool> predictionFetchInFlight_{false};
    QDateTime lastPredictionRefreshAt_;
    QDateTime lastPredictionUpdateAt_;
    QString lastPredictionSystemMessageKey_;
    UniqueAccess<std::optional<PollEvent>> activePoll_;
    std::atomic<bool> pollFetchInFlight_{false};
    QDateTime lastPollRefreshAt_;
    QDateTime lastPollUpdateAt_;
    QString lastPollSystemMessageKey_;
    QString lastPinSystemMessageKey_;
    UniqueAccess<std::optional<ChatWarning>> activeChatWarning_;
    UniqueAccess<std::optional<RaidEvent>> activeRaid_;
    QString locallyClearedRaidId_;
    QDateTime locallyClearedRaidAt_;
    std::atomic<bool> chatWarningFetchInFlight_{false};
    std::atomic<bool> chatWarningFetchNotifyOnError_{false};
    std::atomic<bool> chatWarningAckInFlight_{false};
    QDateTime lastChatWarningRefreshAt_;
    QDateTime lastChatWarningUpdateAt_;
    QDateTime lastChatWarningAuthPromptAt_;
    QString shownChatWarningMessageId_;
    std::atomic<qint64> channelPoints_{-1};
    std::atomic<bool> channelPointsFetchInFlight_{false};
    QDateTime lastChannelPointsRefreshAt_;
    QDateTime lastChannelPointsUpdateAt_;
    QString lastChannelPointsError_;
    bool disconnected_{};
    std::optional<std::chrono::time_point<std::chrono::system_clock>>
        lastConnectedAt_{};
    std::atomic_flag loadingRecentMessages_ = ATOMIC_FLAG_INIT;
    std::unordered_map<QString, std::weak_ptr<MessageThread>> threads_;

protected:
    void messageRemovedFromStart(const MessagePtr &msg) override;

    Atomic<std::shared_ptr<const EmoteMap>> localTwitchEmotes_;
    Atomic<QString> localTwitchEmoteSetID_;
    Atomic<std::shared_ptr<const EmoteMap>> bttvEmotes_;
    Atomic<std::shared_ptr<const EmoteMap>> ffzEmotes_;
    Atomic<std::shared_ptr<const EmoteMap>> seventvEmotes_;
    Atomic<std::optional<EmotePtr>> ffzCustomModBadge_;
    Atomic<std::optional<EmotePtr>> ffzCustomVipBadge_;

    FfzChannelBadgeMap ffzChannelBadges_;
    ThreadGuard tgFfzChannelBadges_;

private:
    // Badges
    UniqueAccess<std::map<QString, std::map<QString, EmotePtr>>>
        badgeSets_;  // "subscribers": { "0": ... "3": ... "6": ...
    UniqueAccess<std::vector<CheerEmoteSet>> cheerEmoteSets_;
    UniqueAccess<std::map<QString, ChannelPointReward>> channelPointRewards_;
    boost::circular_buffer_space_optimized<QueuedRedemption>
        waitingRedemptions_{MAX_QUEUED_REDEMPTIONS};
    boost::circular_buffer_space_optimized<QString>
        recentChannelPointRedemptions_{MAX_RECENT_CHANNEL_POINT_REDEMPTIONS};

    bool mod_ = false;
    bool leadMod_ = false;
    bool leadModStatusKnown_ = false;
    std::atomic<bool> leadModFetchInFlight_{false};
    bool leadModLookupAttempted_ = false;
    QDateTime lastLeadModRefreshAt_;
    bool following_ = false;
    bool followingStatusKnown_ = false;
    std::atomic<bool> followingStatusFetchInFlight_{false};
    QDateTime lastFollowingStatusRefreshAt_;
    QString followingStatusUserId_;
    std::optional<QDateTime> followedAt_;
    bool vip_ = false;
    bool staff_ = false;
    UniqueAccess<QString> roomID_;

    // --
    QString lastSentMessage_;
    QObject lifetimeGuard_;
    QTimer chattersListTimer_;
    QTimer threadClearTimer_;
    QElapsedTimer titleRefreshedTimer_;
    QElapsedTimer clipCreationTimer_;
    bool isClipCreationInProgress{false};

    /**
     * This channels 7TV user-id,
     * empty if this channel isn't connected with 7TV.
     */
    QString seventvUserID_;
    /**
     * This channels current 7TV emote-set-id,
     * empty if this channel isn't connected with 7TV
     */
    QString seventvEmoteSetID_;
    /**
     * The index of the twitch connection in
     * 7TV's user representation.
     */
    size_t seventvUserTwitchConnectionIndex_{};

    /**
     * The next moment in time to signal activity in this channel to 7TV.
     * Or: Up until this moment we don't need to send activity.
     */
    QDateTime nextSeventvActivity_;

    QDateTime nextBttvActivity_;

    /** The platform of the last live emote update ("7TV", "BTTV", "FFZ"). */
    QString lastLiveUpdateEmotePlatform_;
    /** The actor name of the last live emote update. */
    QString lastLiveUpdateEmoteActor_;
    /** A weak reference to the last live emote update message. */
    std::weak_ptr<const Message> lastLiveUpdateMessage_;
    /** A list of the emotes listed in the last live emote update message. */
    std::vector<LiveUpdateEmote> lastLiveUpdateEmotes_;

    /**
     * List of display names of  broadcasters participating in a
     * shared chat session on this channel. The list does not include
     * the broadcaster who owns the channel.
     * This list is passed to the UI for display.
     */
    std::vector<HelixMinimalUser> sharedChatSessionParticipants_;

    /**
     * Set of broadcasterIDs of broadcasters participating in a
     * shared chat session on this channel. The set does not include
     * the broadcaster who owns the channel.
     * This set is used to quickly determine if the participants have
     * changed since the last query of the shared chat session state.
     */
    QSet<QString> sharedChatSessionParticipantIds_;

    /**
     * Timer scheduling the next check of the shared chat session state.
     */
    QTimer nextSharedChatSessionUpdateTimer_;

    /**
     * Time when the next probe of shared chat session state triggered
     * by reception of a shared chat message is allowed.
     * Used to rate-limit Twitch API queries.
     */
    QDateTime nextSharedChatSessionProbe_;

    pajlada::Signals::SignalHolder signalHolder_;
    std::vector<boost::signals2::scoped_connection> bSignals_;

    eventsub::SubscriptionHandle eventSubChannelModerateHandle;
    eventsub::SubscriptionHandle eventSubAutomodMessageHoldHandle;
    eventsub::SubscriptionHandle eventSubAutomodMessageUpdateHandle;
    eventsub::SubscriptionHandle eventSubSuspiciousUserMessageHandle;
    eventsub::SubscriptionHandle eventSubSuspiciousUserUpdateHandle;
    eventsub::SubscriptionHandle eventSubChannelChatUserMessageHoldHandle;
    eventsub::SubscriptionHandle eventSubChannelChatUserMessageUpdateHandle;
    eventsub::SubscriptionHandle eventSubChannelFollowHandle;

    /// May be null if no message is currently pinned.
    std::unique_ptr<const HelixPinnedChatMessage> pinnedMessage_;
    /// Incremented before each getPinnedChatMessage request so that stale
    /// responses from earlier requests are discarded.
    uint64_t pinnedMessageRequestId_ = 0;

    friend class TwitchIrcServer;
    friend class MessageBuilder;
    friend class IrcMessageHandler;
    friend class Commands_E2E_Test;
    friend class TwitchChannelTestAccess;
    friend class ::TestIrcMessageHandlerP;
    friend class ::TestEventSubMessagesP;

    QTimer sendWaitTimer_;
    // Timepoint at which the user can send messages again
    std::optional<std::chrono::steady_clock::time_point> sendWaitEnd_;
};

}  // namespace chatterino

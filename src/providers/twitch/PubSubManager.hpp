// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/liveupdates/Diag.hpp"

#include <pajlada/signals/signal.hpp>
#include <QJsonObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <unordered_map>

#if __has_include(<gtest/gtest_prod.h>)
#    include <gtest/gtest_prod.h>
#endif

namespace chatterino {

class PubSubManagerPrivate;

class PubSub
{
    template <typename T>
    using Signal = pajlada::Signals::Signal<T>;

public:
    PubSub(const QString &host,
           std::chrono::seconds heartbeatInterval = std::chrono::seconds(15));
    ~PubSub();

    PubSub(const PubSub &) = delete;
    PubSub(PubSub &&) = delete;
    PubSub &operator=(const PubSub &) = delete;
    PubSub &operator=(PubSub &&) = delete;

    struct {
        Signal<const QJsonObject &> redeemed;
    } pointReward;

    struct {
        /// leafyrino: full pin payload for system messages / GQL pin state
        Signal<const QJsonObject &> updated;
    } pinnedChat;

    struct {
        Signal<const QJsonObject &> updated;
        Signal<const QJsonObject &> userResult;
    } prediction;

    struct {
        Signal<const QJsonObject &> updated;
    } poll;

    struct {
        Signal<const QJsonObject &> updated;
    } userPoints;

    struct {
        Signal<const QJsonObject &> updated;
    } chatWarning;

    struct {
        Signal<const QJsonObject &> updated;
    } raid;

    /**
     * Listen to incoming channel point redemptions in the given channel.
     * This topic is relevant for everyone.
     *
     * PubSub topic: community-points-channel-v1.{channelID}
     */
    void listenToChannelPointRewards(const QString &channelID);

    /**
     * Listen to real time pin/unpin events in the given channel.
     * This topic is relevant for everyone.
     *
     * PubSub topic: pinned-chat-updates-v1.{channelID}
     */
    void listenToPinnedChatUpdates(const QString &channelID);

    void listenToPredictions(const QString &channelID);
    void listenToPolls(const QString &channelID);
    void listenToRaids(const QString &channelID);
    void listenToUserPredictions(const QString &userID,
                                 const QString &authToken);
    void listenToUserChannelPoints(const QString &userID,
                                   const QString &authToken);
    void listenToChatWarnings(const QString &userID, const QString &authToken);
    void forgetUserAuthenticatedTopics(const QString &userID);
    void forgetOtherUserAuthenticatedTopics(const QString &activeUserID);

    void reconnect();

    struct {
        std::atomic<uint32_t> messagesReceived{0};
        std::atomic<uint32_t> messagesFailedToParse{0};
        std::atomic<uint32_t> failedListenResponses{0};
        std::atomic<uint32_t> listenResponses{0};
        std::atomic<uint32_t> unlistenResponses{0};
    } diag;

    const liveupdates::Diag &wsDiag() const;

private:
    void stop();
    void listenToAuthenticatedTopic(QString topic, const QString &authToken);

    std::unique_ptr<PubSubManagerPrivate> private_;
    std::unordered_map<QString, QString> authenticatedTopicTokens_;

#ifdef FRIEND_TEST
    friend class FTest;

    FRIEND_TEST(TwitchPubSubClient, ServerRespondsToPings);
    FRIEND_TEST(TwitchPubSubClient, ServerDoesntRespondToPings);
    FRIEND_TEST(TwitchPubSubClient, DisconnectedAfter1s);
    FRIEND_TEST(TwitchPubSubClient, ExceedTopicLimit);
    FRIEND_TEST(TwitchPubSubClient, ExceedTopicLimitSingleStep);
#endif
};

}  // namespace chatterino

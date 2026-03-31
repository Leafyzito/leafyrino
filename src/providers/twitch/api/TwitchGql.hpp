// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/twitch/api/Helix.hpp"

#include <QString>

#include <functional>
#include <optional>

class QObject;

namespace chatterino {

/**
 * Undocumented Twitch GraphQL (https://gql.twitch.tv/gql). Used where Helix cannot
 * satisfy the product need (e.g. viewing another channel's predictions). May require
 * Client-Integrity (JWT from POST /integrity), can change without notice, and may have
 * ToS implications — see implementation comments.
 */
namespace TwitchGql {

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
};

/// Fetches active/locked prediction for the channel; maps GQL into HelixPrediction for UI.
/// \a clientId is ignored: gql.twitch.tv rejects most Helix app Client-IDs with HTTP 400;
/// requests use Twitch's public web Client-ID; \a oauthToken is still sent as Bearer.
void fetchPredictionsForChannel(
    const QString &channelId, const QString &channelLogin,
    const QString &clientId, const QString &oauthToken, const QObject *caller,
    std::function<void(std::optional<HelixPrediction>)> onSuccess,
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

}  // namespace TwitchGql

}  // namespace chatterino

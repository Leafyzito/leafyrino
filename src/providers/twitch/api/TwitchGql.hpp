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

/// Fetches active/locked prediction for the channel; maps GQL into HelixPrediction for UI.
/// \a clientId is ignored: gql.twitch.tv rejects most Helix app Client-IDs with HTTP 400;
/// requests use Twitch's public web Client-ID; \a oauthToken is still sent as Bearer.
void fetchPredictionsForChannel(
    const QString &channelId, const QString &channelLogin,
    const QString &clientId, const QString &oauthToken, const QObject *caller,
    std::function<void(std::optional<HelixPrediction>)> onSuccess,
    std::function<void(QString)> onError);

/// Persisted query ChannelPointsContext: viewer's channel points balance for @a channelLogin.
void fetchChannelPointsBalance(const QString &channelLogin,
                               const QString &oauthToken, const QObject *caller,
                               std::function<void(int)> onSuccess,
                               std::function<void(QString)> onError);

}  // namespace TwitchGql

}  // namespace chatterino

// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <boost/functional/hash.hpp>
#include <QHash>
#include <QString>

#include <utility>
#include <vector>

namespace chatterino::eventsub {

struct SubscriptionRequest {
    QString subscriptionType;

    QString subscriptionVersion;

    QString ownerTwitchUserID;

    std::vector<std::pair<QString, QString>> conditions;

    /// Optional Helix auth override for subscription creation (not part of
    /// request identity).
    QString helixClientId;
    QString helixOAuthToken;

    friend QDebug operator<<(QDebug dbg, const SubscriptionRequest &v);
};

bool operator==(const SubscriptionRequest &lhs, const SubscriptionRequest &rhs);
bool operator!=(const SubscriptionRequest &lhs, const SubscriptionRequest &rhs);

}  // namespace chatterino::eventsub

namespace std {

template <>
struct hash<chatterino::eventsub::SubscriptionRequest> {
    size_t operator()(const chatterino::eventsub::SubscriptionRequest &v) const
    {
        size_t seed = 0;

        boost::hash_combine(seed, qHash(v.subscriptionType));
        boost::hash_combine(seed, qHash(v.subscriptionVersion));

        for (const auto &[conditionKey, conditionValue] : v.conditions)
        {
            boost::hash_combine(seed, qHash(conditionKey));
            boost::hash_combine(seed, qHash(conditionValue));
        }

        return seed;
    }
};

}  // namespace std

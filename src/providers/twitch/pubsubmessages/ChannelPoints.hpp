#pragma once

#include <magic_enum/magic_enum.hpp>
#include <QJsonObject>
#include <QString>

namespace chatterino {

struct PubSubCommunityPointsChannelV1Message {
    enum class Type {
        AutomaticRewardRedeemed,
        RewardRedeemed,

        INVALID,
    };

    QString typeString;
    Type type = Type::INVALID;

    QJsonObject data;

    PubSubCommunityPointsChannelV1Message(const QJsonObject &root);
};

struct PubSubCommunityPointsUserV1Message {
    enum class Type {
        PointsEarned,
        PointsSpent,
        ClaimAvailable,

        INVALID,
    };

    QString typeString;
    Type type = Type::INVALID;

    QJsonObject data;

    PubSubCommunityPointsUserV1Message(const QJsonObject &root);
};

}

template <>
constexpr magic_enum::customize::customize_t magic_enum::customize::enum_name<
    chatterino::PubSubCommunityPointsChannelV1Message::Type>(
    chatterino::PubSubCommunityPointsChannelV1Message::Type value) noexcept
{
    switch (value)
    {
        case chatterino::PubSubCommunityPointsChannelV1Message::Type::
            AutomaticRewardRedeemed:
            return "automatic-reward-redeemed";
        case chatterino::PubSubCommunityPointsChannelV1Message::Type::
            RewardRedeemed:
            return "reward-redeemed";
        default:
            return default_tag;
    }
}

template <>
constexpr magic_enum::customize::customize_t magic_enum::customize::enum_name<
    chatterino::PubSubCommunityPointsUserV1Message::Type>(
    chatterino::PubSubCommunityPointsUserV1Message::Type value) noexcept
{
    switch (value)
    {
        case chatterino::PubSubCommunityPointsUserV1Message::Type::PointsEarned:
            return "points-earned";
        case chatterino::PubSubCommunityPointsUserV1Message::Type::PointsSpent:
            return "points-spent";
        case chatterino::PubSubCommunityPointsUserV1Message::Type::ClaimAvailable:
            return "claim-available";
        default:
            return default_tag;
    }
}

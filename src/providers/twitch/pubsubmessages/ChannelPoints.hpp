#pragma once

#include <magic_enum/magic_enum.hpp>
#include <QJsonObject>
#include <QString>

namespace chatterino {

struct PubSubCommunityPointsChannelV1Message {
    enum class Type {
        AutomaticRewardRedeemed,
        RewardRedeemed,
        RedemptionStatusUpdate,
        UpdateRedemptionStatusesProgress,
        UpdateRedemptionStatusesFinished,

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

}  // namespace chatterino

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
        case chatterino::PubSubCommunityPointsChannelV1Message::Type::
            RedemptionStatusUpdate:
            return "redemption-status-update";
        case chatterino::PubSubCommunityPointsChannelV1Message::Type::
            UpdateRedemptionStatusesProgress:
            return "update-redemption-statuses-progress";
        case chatterino::PubSubCommunityPointsChannelV1Message::Type::
            UpdateRedemptionStatusesFinished:
            return "update-redemption-statuses-finished";
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
        case chatterino::PubSubCommunityPointsUserV1Message::Type::
            ClaimAvailable:
            return "claim-available";
        default:
            return default_tag;
    }
}

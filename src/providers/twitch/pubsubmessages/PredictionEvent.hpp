#pragma once

#include <magic_enum/magic_enum.hpp>
#include <QJsonObject>
#include <QString>

namespace chatterino {

struct PubSubPredictionChannelV1Message {
    enum class Type {
        EventCreated,
        EventUpdated,
        EventLocked,
        EventResolved,
        EventCanceled,

        INVALID,
    };

    QString typeString;
    Type type = Type::INVALID;

    QJsonObject data;

    PubSubPredictionChannelV1Message(const QJsonObject &root);
};

}  // namespace chatterino

template <>
constexpr magic_enum::customize::customize_t magic_enum::customize::enum_name<
    chatterino::PubSubPredictionChannelV1Message::Type>(
    chatterino::PubSubPredictionChannelV1Message::Type value) noexcept
{
    switch (value)
    {
        case chatterino::PubSubPredictionChannelV1Message::Type::EventCreated:
            return "event-created";
        case chatterino::PubSubPredictionChannelV1Message::Type::EventUpdated:
            return "event-updated";
        case chatterino::PubSubPredictionChannelV1Message::Type::EventLocked:
            return "event-locked";
        case chatterino::PubSubPredictionChannelV1Message::Type::EventResolved:
            return "event-resolved";
        case chatterino::PubSubPredictionChannelV1Message::Type::EventCanceled:
            return "event-canceled";
        default:
            return default_tag;
    }
}

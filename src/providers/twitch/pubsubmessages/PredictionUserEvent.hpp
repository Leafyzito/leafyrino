#pragma once

#include <magic_enum/magic_enum.hpp>
#include <QJsonObject>
#include <QString>

namespace chatterino {

struct PubSubPredictionUserV1Message {
    enum class Type {
        PredictionMade,
        PredictionResult,

        INVALID,
    };

    QString typeString;
    Type type = Type::INVALID;

    QJsonObject data;

    PubSubPredictionUserV1Message(const QJsonObject &root);
};

}  // namespace chatterino

template <>
constexpr magic_enum::customize::customize_t magic_enum::customize::enum_name<
    chatterino::PubSubPredictionUserV1Message::Type>(
    chatterino::PubSubPredictionUserV1Message::Type value) noexcept
{
    switch (value)
    {
        case chatterino::PubSubPredictionUserV1Message::Type::PredictionMade:
            return "prediction-made";
        case chatterino::PubSubPredictionUserV1Message::Type::PredictionResult:
            return "prediction-result";
        default:
            return default_tag;
    }
}

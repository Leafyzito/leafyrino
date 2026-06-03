#pragma once

#include <magic_enum/magic_enum.hpp>
#include <QJsonObject>
#include <QString>

namespace chatterino {

struct PubSubPinnedChatUpdatesV1Message {
    enum class Type {
        Pin,
        Update,
        Unpin,

        INVALID,
    };

    QString typeString;
    Type type = Type::INVALID;

    QJsonObject data;

    PubSubPinnedChatUpdatesV1Message(const QJsonObject &root);
};

}  // namespace chatterino

template <>
constexpr magic_enum::customize::customize_t magic_enum::customize::enum_name<
    chatterino::PubSubPinnedChatUpdatesV1Message::Type>(
    chatterino::PubSubPinnedChatUpdatesV1Message::Type value) noexcept
{
    switch (value)
    {
        case chatterino::PubSubPinnedChatUpdatesV1Message::Type::Pin:
            return "pin-message";
        case chatterino::PubSubPinnedChatUpdatesV1Message::Type::Update:
            return "update-message";
        case chatterino::PubSubPinnedChatUpdatesV1Message::Type::Unpin:
            return "unpin-message";
        default:
            return default_tag;
    }
}

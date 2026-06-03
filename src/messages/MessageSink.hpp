// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/enums/MessageContext.hpp"
#include "common/FlagsEnum.hpp"
#include "messages/MessageFlag.hpp"

#include <memory>
#include <optional>

class QStringView;
class QDateTime;

namespace chatterino {

struct Message;
using MessagePtr = std::shared_ptr<const Message>;

enum class MessageSinkTrait : uint8_t {
    None = 0,

    AddMentionsToGlobalChannel = 1 << 0,

    RequiresKnownChannelPointReward = 1 << 1,
};
using MessageSinkTraits = FlagsEnum<MessageSinkTrait>;

class MessageSink
{
public:
    virtual ~MessageSink() = default;

    virtual void addMessage(
        MessagePtr message, MessageContext ctx,
        std::optional<MessageFlags> overridingFlags = std::nullopt) = 0;

    virtual void addOrReplaceTimeout(MessagePtr clearchatMessage,
                                     const QDateTime &now) = 0;

    virtual void addOrReplaceClearChat(MessagePtr clearchatMessage,
                                       const QDateTime &now) = 0;

    virtual void disableAllMessages() = 0;

    virtual void applySimilarityFilters(const MessagePtr &message) const = 0;

    virtual MessagePtr findMessageByID(QStringView id) = 0;

    virtual MessageSinkTraits sinkTraits() const = 0;
};

}  // namespace chatterino

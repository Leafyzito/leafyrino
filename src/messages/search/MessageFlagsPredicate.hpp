// SPDX-FileCopyrightText: 2021 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/FlagsEnum.hpp"
#include "messages/Message.hpp"
#include "messages/search/MessagePredicate.hpp"

namespace chatterino {

using MessageFlags = FlagsEnum<MessageFlag>;

class MessageFlagsPredicate : public MessagePredicate
{
public:

    MessageFlagsPredicate(const QString &flags, bool negate);

protected:

    bool appliesToImpl(const Message &message) override;

private:

    MessageFlags flags_;
};

}

// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Message.hpp"
#include "messages/search/MessagePredicate.hpp"

#include <QString>

#include <vector>

namespace chatterino {

class ClientPredicate : public MessagePredicate
{
public:
    ClientPredicate(const QString &clients, bool negate);

protected:
    bool appliesToImpl(const Message &message) override;

private:
    std::vector<Message::ClientDetectionStatus> clients_;
};

}  // namespace chatterino

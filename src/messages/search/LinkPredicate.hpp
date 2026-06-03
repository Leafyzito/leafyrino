// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/search/MessagePredicate.hpp"

#include <QString>

namespace chatterino {

class LinkPredicate : public MessagePredicate
{
public:

    LinkPredicate(bool negate);

protected:

    bool appliesToImpl(const Message &message) override;
};

}

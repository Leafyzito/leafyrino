// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <memory>

namespace chatterino {

struct Message;

class MessagePredicate
{
public:
    virtual ~MessagePredicate() = default;

    bool appliesTo(const Message &message)
    {
        auto result = this->appliesToImpl(message);
        if (this->isNegated_)
        {
            return !result;
        }
        return result;
    }

protected:
    explicit MessagePredicate(bool negate)
        : isNegated_(negate)
    {
    }

    virtual bool appliesToImpl(const Message &message) = 0;

private:
    const bool isNegated_ = false;
};
}

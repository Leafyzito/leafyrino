// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/search/MessagePredicate.hpp"

#include <QString>

namespace chatterino {

class SubstringPredicate : public MessagePredicate
{
public:

    SubstringPredicate(const QString &search);

protected:

    bool appliesToImpl(const Message &message) override;

private:

    const QString search_;
};

}

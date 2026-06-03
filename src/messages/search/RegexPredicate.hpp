// SPDX-FileCopyrightText: 2021 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/search/MessagePredicate.hpp"

#include <QRegularExpression>
#include <QString>

namespace chatterino {

class RegexPredicate : public MessagePredicate
{
public:
    RegexPredicate(const QString &regex, bool negate);

protected:
    bool appliesToImpl(const Message &message) override;

private:
    QRegularExpression regex_;
};

}  // namespace chatterino

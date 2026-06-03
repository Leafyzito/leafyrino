// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/search/MessagePredicate.hpp"

#include <QString>
#include <QStringList>

namespace chatterino {

class BadgePredicate : public MessagePredicate
{
public:
    BadgePredicate(const QString &badges, bool negate);

protected:
    bool appliesToImpl(const Message &message) override;

private:
    QStringList badges_;
};

}  // namespace chatterino

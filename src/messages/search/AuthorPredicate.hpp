// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/search/MessagePredicate.hpp"

#include <QString>
#include <QStringList>

namespace chatterino {

class AuthorPredicate : public MessagePredicate
{
public:

    AuthorPredicate(const QString &authors, bool negate);

protected:

    bool appliesToImpl(const Message &message) override;

private:

    QStringList authors_;
};

}

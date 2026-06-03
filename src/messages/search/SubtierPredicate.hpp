// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/search/MessagePredicate.hpp"

#include <QString>
#include <QStringList>

namespace chatterino {

class SubtierPredicate : public MessagePredicate
{
public:
    SubtierPredicate(const QString &subtiers, bool negate);

protected:
    bool appliesToImpl(const Message &message) override;

private:
    QStringList subtiers_;
};

}  // namespace chatterino

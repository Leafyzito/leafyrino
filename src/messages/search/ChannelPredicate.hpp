// SPDX-FileCopyrightText: 2021 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/search/MessagePredicate.hpp"

#include <QString>
#include <QStringList>

namespace chatterino {

class ChannelPredicate : public MessagePredicate
{
public:
    ChannelPredicate(const QString &channels, bool negate);

protected:
    bool appliesToImpl(const Message &message) override;

private:
    QStringList channels_;
};

}  // namespace chatterino

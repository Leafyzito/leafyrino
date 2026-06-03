// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/MessageElement.hpp"

#include <QString>

namespace chatterino {

class TwitchBadge
{
public:
    TwitchBadge(QString key, QString value);

    bool operator==(const TwitchBadge &other) const;

    QString key_;
    QString value_;

    MessageElementFlag flag_{
        MessageElementFlag::BadgeVanity};
};

}

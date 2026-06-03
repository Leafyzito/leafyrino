// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Emote.hpp"
#include "providers/twitch/TwitchBadge.hpp"

#include <QString>
#include <QVariantMap>

#include <unordered_map>

namespace chatterino {

struct TwitchEmoteOccurrence {
    int start;
    int end;
    EmotePtr ptr;
    EmoteName name;

    bool operator==(const TwitchEmoteOccurrence &other) const
    {
        return std::tie(this->start, this->end, this->ptr, this->name) ==
               std::tie(other.start, other.end, other.ptr, other.name);
    }
};

std::unordered_map<QString, QString> parseBadgeInfoTag(const QVariantMap &tags);

std::vector<TwitchBadge> parseBadgeTag(const QVariantMap &tags,
                                       const QString &tagName = "badges");

std::vector<TwitchEmoteOccurrence> parseTwitchEmotes(const QVariantMap &tags,
                                                     const QString &content,
                                                     int messageOffset);

}  // namespace chatterino

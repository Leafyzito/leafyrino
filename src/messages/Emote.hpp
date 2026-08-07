// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"
#include "messages/ImageSet.hpp"

#include <QStringList>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

class QJsonObject;

namespace chatterino {

struct Emote {
    EmoteName name;
    ImageSet images;
    Tooltip tooltip;
    Url homePage;
    bool zeroWidth{};
    EmoteId id;
    EmoteAuthor author;
    /**
     * If this emote is aliased, this contains
     * the original (base) name of the emote.
     */
    std::optional<EmoteName> baseName;
    QStringList tags;

    const QString &getCopyString() const
    {
        return this->name.string;
    }

    QJsonObject toJson() const;
};

bool operator==(const Emote &a, const Emote &b);
bool operator!=(const Emote &a, const Emote &b);

using EmotePtr = std::shared_ptr<const Emote>;

class EmoteMap : public std::unordered_map<EmoteName, EmotePtr>
{
public:
    EmoteMap::const_iterator findEmote(const QString &emoteNameHint,
                                       const QString &emoteID) const;
};

inline const std::shared_ptr<const EmoteMap> EMPTY_EMOTE_MAP =
    std::make_shared<const EmoteMap>();

EmotePtr cachedOrMakeEmotePtr(Emote &&emote, const EmoteMap &cache);
EmotePtr cachedOrMakeEmotePtr(
    Emote &&emote,
    std::unordered_map<EmoteId, std::weak_ptr<const Emote>> &cache,
    std::mutex &mutex, const EmoteId &id);

}  // namespace chatterino

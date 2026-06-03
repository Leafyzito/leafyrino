// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"
#include "common/UniqueAccess.hpp"
#include "providers/twitch/TwitchUser.hpp"

#include <boost/unordered/unordered_flat_map_fwd.hpp>
#include <QColor>
#include <QRegularExpression>
#include <QString>

#include <memory>
#include <unordered_map>

namespace chatterino {

inline constexpr QStringView TWITCH_EMOTE_TEMPLATE =
    u"https://static-cdn.jtvnw.net/emoticons/v2/%1/default/dark/%2";

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

struct CheerEmote {
    QColor color;
    int minBits;
    QRegularExpression regex;

    EmotePtr animatedEmote;
    EmotePtr staticEmote;
};

struct CheerEmoteSet {
    QRegularExpression regex;
    std::vector<CheerEmote> cheerEmotes;
};

struct TwitchEmoteSet {

    std::shared_ptr<TwitchUser> owner;

    std::vector<EmotePtr> emotes;

    bool isBits = false;

    bool isSubLike = false;

    QString title() const;
};
using TwitchEmoteSetMap = boost::unordered_flat_map<EmoteSetId, TwitchEmoteSet>;

struct HelixChannelEmote;

inline constexpr QStringView TWITCH_SUB_EMOTE_SET_PREFIX = u"x-c2-s-";
inline constexpr QStringView TWITCH_BIT_EMOTE_SET_PREFIX = u"x-c2-b-";

struct TwitchEmoteSetMeta {
    QString setID;

    bool isBits = false;

    bool isSubLike = false;
};

TwitchEmoteSetMeta getTwitchEmoteSetMeta(const HelixChannelEmote &emote);

class ITwitchEmotes
{
public:
    virtual ~ITwitchEmotes() = default;

    virtual EmotePtr getOrCreateEmote(const EmoteId &id,
                                      const EmoteName &name) = 0;
};

class TwitchEmotes : public ITwitchEmotes
{
public:
    static QString cleanUpEmoteCode(const QString &dirtyEmoteCode);
    TwitchEmotes() = default;

    EmotePtr getOrCreateEmote(const EmoteId &id,
                              const EmoteName &name) override;

private:
    UniqueAccess<std::unordered_map<EmoteId, std::weak_ptr<Emote>>>
        twitchEmotesCache_;
};

}

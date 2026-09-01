// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"
#include "common/Atomic.hpp"
#include "common/FlagsEnum.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace chatterino {

class ImageSet;
class Channel;
class KickChannel;
namespace seventv::eventapi {
struct EmoteAddDispatch;
struct EmoteUpdateDispatch;
struct EmoteRemoveDispatch;
}  // namespace seventv::eventapi

enum class SeventvActiveEmoteFlag : std::int64_t {
    None = 0LL,

    ZeroWidth = (1LL << 0),

    OverrideTwitchGlobal = (1 << 16),

    OverrideTwitchSubscriber = (1 << 17),

    OverrideBetterTTV = (1 << 18),
};

enum class SeventvEmoteFlag : int64_t {
    None = 0LL,

    Private = 1 << 0,

    Authentic = (1LL << 1),

    ZeroWidth = (1LL << 8),

    ContentSexual = (1LL << 16),

    ContentEpilepsy = (1LL << 17),

    ContentEdgy = (1 << 18),

    ContentTwitchDisallowed = (1LL << 24),
};

using SeventvActiveEmoteFlags = FlagsEnum<SeventvActiveEmoteFlag>;
using SeventvEmoteFlags = FlagsEnum<SeventvEmoteFlag>;

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;
class EmoteMap;

enum class SeventvEmoteSetKind : uint8_t {
    Global,
    Personal,
    Channel,
};

enum class SeventvEmoteSetFlag : uint32_t {
    Immutable = (1 << 0),
    Privileged = (1 << 1),
    Personal = (1 << 2),
    Commercial = (1 << 3),
};
using SeventvEmoteSetFlags = FlagsEnum<SeventvEmoteSetFlag>;

namespace seventv::detail {

EmoteMap parseEmotes(const QJsonArray &emoteSetEmotes,
                     SeventvEmoteSetKind kind);

}

class SeventvEmotes final
{
public:
    struct ChannelInfo {
        QString userID;
        QString emoteSetID;
        size_t twitchConnectionIndex;
    };

    SeventvEmotes();

    std::shared_ptr<const EmoteMap> globalEmotes() const;
    std::optional<EmotePtr> globalEmote(EmoteNameView name) const;
    void loadGlobalEmotes();
    void setGlobalEmotes(std::shared_ptr<const EmoteMap> emotes);
    static void loadChannelEmotes(
        const std::weak_ptr<Channel> &channel, const QString &channelId,
        std::function<void(EmoteMap &&, ChannelInfo)> callback,
        bool manualRefresh, bool cacheHit);
    static void loadKickChannelEmotes(
        const std::weak_ptr<KickChannel> &channel, uint64_t userID,
        std::function<void(EmoteMap &&, ChannelInfo)> callback,
        bool manualRefresh, bool cacheHit);

    static std::optional<EmotePtr> addEmote(
        Atomic<std::shared_ptr<const EmoteMap>> &map,
        const seventv::eventapi::EmoteAddDispatch &dispatch,
        SeventvEmoteSetKind kind = SeventvEmoteSetKind::Channel);

    static std::optional<EmotePtr> updateEmote(
        Atomic<std::shared_ptr<const EmoteMap>> &map,
        const seventv::eventapi::EmoteUpdateDispatch &dispatch,
        SeventvEmoteSetKind kind = SeventvEmoteSetKind::Channel);

    static std::optional<EmotePtr> removeEmote(
        Atomic<std::shared_ptr<const EmoteMap>> &map,
        const seventv::eventapi::EmoteRemoveDispatch &dispatch);

    static void getEmoteSet(
        const QString &emoteSetId,
        std::function<void(EmoteMap &&, QString)> successCallback,
        std::function<void(QString)> errorCallback);

    static ImageSet createImageSet(const QJsonObject &emoteData,
                                   bool useStatic);

private:
    Atomic<std::shared_ptr<const EmoteMap>> global_;

    std::vector<std::unique_ptr<pajlada::Signals::ScopedConnection>>
        managedConnections;
};

}  // namespace chatterino

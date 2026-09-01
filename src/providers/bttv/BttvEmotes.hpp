// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"
#include "common/Atomic.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QJsonObject>
#include <QString>

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;
class EmoteMap;
class Channel;
struct BttvLiveUpdateEmoteUpdateAddMessage;
struct BttvLiveUpdateEmoteRemoveMessage;

namespace bttv::detail {

EmoteMap parseChannelEmotes(const QJsonObject &jsonRoot,
                            const QString &channelDisplayName);

}

class BttvEmotes final
{
    static constexpr const char *globalEmoteApiUrl =
        "https://api.betterttv.net/3/cached/emotes/global";
    static constexpr const char *bttvChannelEmoteApiUrl =
        "https://api.betterttv.net/3/cached/users/twitch/";

public:
    BttvEmotes();

    std::shared_ptr<const EmoteMap> emotes() const;
    std::optional<EmotePtr> emote(EmoteNameView name) const;
    void loadEmotes();
    void setEmotes(std::shared_ptr<const EmoteMap> emotes);
    static void loadChannel(std::weak_ptr<Channel> channel,
                            const QString &channelId,
                            const QString &channelDisplayName,
                            std::function<void(EmoteMap &&)> callback,
                            bool manualRefresh, bool cacheHit);

    static EmotePtr addEmote(
        const QString &channelDisplayName,
        Atomic<std::shared_ptr<const EmoteMap>> &channelEmoteMap,
        const BttvLiveUpdateEmoteUpdateAddMessage &message);

    static std::optional<std::pair<EmotePtr, EmotePtr>> updateEmote(
        const QString &channelDisplayName,
        Atomic<std::shared_ptr<const EmoteMap>> &channelEmoteMap,
        const BttvLiveUpdateEmoteUpdateAddMessage &message);

    static std::optional<EmotePtr> removeEmote(
        Atomic<std::shared_ptr<const EmoteMap>> &channelEmoteMap,
        const BttvLiveUpdateEmoteRemoveMessage &message);

private:
    Atomic<std::shared_ptr<const EmoteMap>> global_;

    std::vector<std::unique_ptr<pajlada::Signals::ScopedConnection>>
        managedConnections;
};

}  // namespace chatterino

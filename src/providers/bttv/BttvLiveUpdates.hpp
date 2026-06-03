// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <pajlada/signals/signal.hpp>
#include <QString>

#include <memory>

namespace chatterino {

namespace liveupdates {
struct Diag;
}

struct BttvLiveUpdateEmoteUpdateAddMessage;
struct BttvLiveUpdateEmoteRemoveMessage;

class BttvLiveUpdatesPrivate;
class BttvLiveUpdates
{
    template <typename T>
    using Signal = pajlada::Signals::Signal<T>;

public:
    BttvLiveUpdates(QString host);
    ~BttvLiveUpdates();

    struct {
        Signal<BttvLiveUpdateEmoteUpdateAddMessage> emoteAdded;
        Signal<BttvLiveUpdateEmoteUpdateAddMessage> emoteUpdated;
        Signal<BttvLiveUpdateEmoteRemoveMessage> emoteRemoved;
    } signals_;

    void joinChannel(const QString &channelID, const QString &userID);

    void broadcastMe(const QString &channelID, const QString &userID);

    void partChannel(const QString &id);

    void stop();

    const liveupdates::Diag &diag() const;

private:
    std::unique_ptr<BttvLiveUpdatesPrivate> private_;
};

}  // namespace chatterino

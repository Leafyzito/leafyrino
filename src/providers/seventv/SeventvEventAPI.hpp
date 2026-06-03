// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
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

namespace seventv::eventapi {
struct EmoteAddDispatch;
struct EmoteUpdateDispatch;
struct EmoteRemoveDispatch;
struct UserConnectionUpdateDispatch;
struct PersonalEmoteSetAdded;
}

class SeventvBadges;
class SeventvPaints;
class EmoteMap;

class SeventvEventAPIPrivate;
class SeventvEventAPI
{
    template <typename T>
    using Signal = pajlada::Signals::Signal<T>;

public:
    SeventvEventAPI(QString host,
                    std::chrono::milliseconds defaultHeartbeatInterval =
                        std::chrono::milliseconds(25000));
    ~SeventvEventAPI();

    struct {
        Signal<const seventv::eventapi::EmoteAddDispatch &> emoteAdded;
        Signal<const seventv::eventapi::EmoteUpdateDispatch &> emoteUpdated;
        Signal<const seventv::eventapi::EmoteRemoveDispatch &> emoteRemoved;
        Signal<const seventv::eventapi::UserConnectionUpdateDispatch &>
            userUpdated;
        Signal<const seventv::eventapi::PersonalEmoteSetAdded &>
            personalEmoteSetAdded;
    } signals_;

    void subscribeUser(const QString &userID, const QString &emoteSetID);

    void subscribeTwitchChannel(const QString &id);
    void subscribeKickChannel(const QString &id);

    void subscribePlatformChannel(const QString &userID,
                                  const QString &platform);

    void unsubscribeUser(const QString &id);

    void unsubscribeEmoteSet(const QString &id);

    void unsubscribeTwitchChannel(const QString &id);
    void unsubscribeKickChannel(const QString &id);

    void unsubscribePlatformChannel(const QString &userID,
                                    const QString &platform);

    void stop();

    const liveupdates::Diag &diag() const;

private:
    std::unique_ptr<SeventvEventAPIPrivate> private_;
};

}

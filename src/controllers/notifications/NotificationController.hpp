// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/ChatterinoSetting.hpp"
#include "common/SignalVector.hpp"
#include "util/QCompareTransparent.hpp"

#include <QTimer>

namespace chatterino {

class Settings;
class Paths;
struct HelixStream;

class NotificationModel;

enum class Platform : uint8_t {
    Twitch,
};

class NotificationController final
{
public:
    NotificationController();

    void initialize();

    bool isChannelNotified(const QString &channelName, Platform p) const;
    void updateChannelNotification(const QString &channelName, Platform p);
    void addChannelNotification(const QString &channelName, Platform p);
    void removeChannelNotification(const QString &channelName, Platform p);

    struct NotificationPayload {
        QString channelId;
        QString channelName;
        QString displayName;
        QString title;
        bool isInitialUpdate = false;
    };

    void notifyTwitchChannelLive(const NotificationPayload &payload) const;

    void notifyTwitchChannelOffline(const QString &id) const;

    void playSound() const;

    NotificationModel *createModel(QObject *parent, Platform p);

private:
    void fetchFakeChannels();
    void removeFakeChannel(const QString &channelName);
    void updateFakeChannel(const QString &channelName,
                           const std::optional<HelixStream> &stream);

    struct FakeChannel {
        QString id;
        bool isLive = false;
    };

    std::map<QString, FakeChannel, QCompareCaseInsensitive> fakeChannels_;

    QTimer liveStatusTimer_;

    std::map<Platform, SignalVector<QString>> channelMap;

    ChatterinoSetting<std::vector<QString>> twitchSetting_ = {
        "/notifications/twitch"};
};

}

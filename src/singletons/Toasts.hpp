// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <pajlada/settings/setting.hpp>
#include <QString>

namespace chatterino {

enum class ToastReaction {
    OpenInBrowser = 0,
    OpenInPlayer = 1,
    OpenInStreamlink = 2,
    DontOpen = 3,
    OpenInCustomPlayer = 4,
};

class Toasts final
{
public:
    ~Toasts();

    void sendChannelNotification(const QString &channelName,
                                 const QString &channelTitle);
    bool sendHighlightNotification(const QString &channelName,
                                   const QString &title, const QString &body,
                                   const QString &messageId);
    static bool isHighlightNotificationSupported();
    static QString findStringFromReaction(const ToastReaction &reaction);
    static QString findStringFromReaction(
        const pajlada::Settings::Setting<int> &reaction);

    static bool isEnabled();

private:
#ifdef Q_OS_WIN
    void ensureInitialized();
    bool ensureActionCenterActivation();
    void sendWindowsNotification(const QString &channelName,
                                 const QString &channelTitle);
    bool sendWindowsHighlightNotification(const QString &channelName,
                                          const QString &title,
                                          const QString &body,
                                          const QString &messageId);

    bool initialized_ = false;
    bool actionCenterActivationChecked_ = false;
    bool actionCenterActivationAvailable_ = false;
#elif defined(CHATTERINO_WITH_LIBNOTIFY)
    void ensureInitialized();
    void sendLibnotify(const QString &channelName, const QString &channelTitle);
    bool sendLibnotifyHighlightNotification(const QString &channelName,
                                            const QString &title,
                                            const QString &body,
                                            const QString &messageId);

    bool initialized_ = false;
#endif
};
}  // namespace chatterino

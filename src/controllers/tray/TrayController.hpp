#pragma once

#include "messages/Message.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QDateTime>
#include <QObject>
#include <QString>

#include <deque>

class QAction;
class QMenu;
class QSystemTrayIcon;

namespace chatterino {

class Channel;
class WindowManager;

class TrayController final : public QObject
{
public:
    explicit TrayController(WindowManager &windows);
    ~TrayController() override;

    bool canHideToTray() const;
    bool isHiddenToTray() const;
    void hideToTray();
    void restoreFromTray();
    void markWindowShown();

    void notifyHighlight(const Channel *channel, const MessagePtr &message,
                         bool playSound);

    void openRecentPing(int index);

private:
    struct RecentPing {
        QString channelName;
        QString displayName;
        QString messageText;
        QString messageId;
        QDateTime createdAt;
    };

    void ensureTrayIcon();
    void updateTrayVisibility();
    void refreshMenu();
    void rebuildRecentPingMenu();
    void showTrayPingNotification(const RecentPing &ping);
    bool notificationsMuted() const;
    QString formatPingActionText(const RecentPing &ping) const;

    WindowManager &windows_;

    QSystemTrayIcon *trayIcon_ = nullptr;
    QMenu *menu_ = nullptr;
    QMenu *recentPingsMenu_ = nullptr;
    QAction *openAction_ = nullptr;
    QAction *openLastPingAction_ = nullptr;
    QAction *mutePingsAction_ = nullptr;
    QAction *pingNotificationsAction_ = nullptr;
    QAction *quitAction_ = nullptr;

    std::deque<RecentPing> recentPings_;
    QDateTime mutePingsUntil_;
    bool hiddenToTray_ = false;
    bool notifiedHiddenHint_ = false;

    pajlada::Signals::SignalHolder signalHolder_;
};

}  // namespace chatterino

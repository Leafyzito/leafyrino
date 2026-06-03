#include "controllers/tray/TrayController.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Toasts.hpp"
#include "singletons/WindowManager.hpp"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QMenu>
#include <QSignalBlocker>
#include <QSystemTrayIcon>

namespace {

constexpr qsizetype MAX_PREVIEW_LENGTH = 160;
constexpr size_t MAX_RECENT_PINGS = 10;

QString cleanForTray(QString text)
{
    text.replace('\n', ' ');
    text.replace('\r', ' ');
    text = text.simplified();
    if (text.size() > MAX_PREVIEW_LENGTH)
    {
        text = text.left(MAX_PREVIEW_LENGTH - 1) + QStringLiteral("...");
    }
    return text;
}

QString pingDisplayName(const chatterino::MessagePtr &message)
{
    if (message == nullptr)
    {
        return {};
    }

    auto displayName = message->displayName.trimmed();
    if (displayName.isEmpty())
    {
        displayName = message->localizedName.trimmed();
    }
    if (displayName.isEmpty())
    {
        displayName = message->loginName.trimmed();
    }
    return displayName;
}

}

namespace chatterino {

TrayController::TrayController(WindowManager &windows)
    : windows_(windows)
{
    this->ensureTrayIcon();

    getSettings()->trayHideOnClose.connect(
        [this](bool) {
            this->updateTrayVisibility();
        },
        this->signalHolder_);

    getSettings()->trayNotifyOnSoundHighlights.connect(
        [this](bool) {
            this->refreshMenu();
        },
        this->signalHolder_);
}

TrayController::~TrayController()
{
    if (this->trayIcon_ != nullptr)
    {
        this->trayIcon_->setContextMenu(nullptr);
        this->trayIcon_->hide();
    }

    delete this->menu_;
}

bool TrayController::canHideToTray() const
{
    return getSettings()->trayHideOnClose &&
           QSystemTrayIcon::isSystemTrayAvailable() &&
           this->trayIcon_ != nullptr;
}

bool TrayController::isHiddenToTray() const
{
    return this->hiddenToTray_;
}

void TrayController::hideToTray()
{
    if (!this->canHideToTray())
    {
        return;
    }

    this->hiddenToTray_ = true;
    this->updateTrayVisibility();

    if (!this->notifiedHiddenHint_)
    {
        this->notifiedHiddenHint_ = true;
        if (this->trayIcon_ != nullptr &&
            QSystemTrayIcon::supportsMessages())
        {
            this->trayIcon_->showMessage(
                QStringLiteral("Leafyrino is still running"),
                QStringLiteral(
                    "You can disable this feature from the settings."),
                QSystemTrayIcon::Information, 7000);
        }
    }
}

void TrayController::restoreFromTray()
{
    this->hiddenToTray_ = false;
    this->windows_.showMainWindow();
    this->updateTrayVisibility();
}

void TrayController::markWindowShown()
{
    if (!this->hiddenToTray_)
    {
        return;
    }

    this->hiddenToTray_ = false;
    this->updateTrayVisibility();
}

void TrayController::notifyHighlight(const Channel *channel,
                                     const MessagePtr &message,
                                     bool playSound)
{
    if (!playSound || !this->hiddenToTray_ ||
        !getSettings()->trayNotifyOnSoundHighlights ||
        this->notificationsMuted() || channel == nullptr ||
        message == nullptr || channel->getType() != Channel::Type::Twitch)
    {
        return;
    }

    RecentPing ping;
    ping.channelName = channel->getName();
    ping.displayName = pingDisplayName(message);
    ping.messageText = cleanForTray(message->messageText);
    ping.messageId = message->id;
    ping.createdAt = QDateTime::currentDateTimeUtc();

    this->recentPings_.push_front(ping);
    while (this->recentPings_.size() > MAX_RECENT_PINGS)
    {
        this->recentPings_.pop_back();
    }

    this->refreshMenu();
    this->showTrayPingNotification(ping);
}

void TrayController::openRecentPing(int index)
{
    if (index < 0 || index >= static_cast<int>(this->recentPings_.size()))
    {
        this->restoreFromTray();
        return;
    }

    const auto ping = this->recentPings_[static_cast<size_t>(index)];
    this->hiddenToTray_ = false;
    this->windows_.openChannelOrMessageFromTray(ping.channelName,
                                                ping.messageId);
    this->updateTrayVisibility();
}

void TrayController::ensureTrayIcon()
{
    if (this->trayIcon_ != nullptr)
    {
        return;
    }

    this->trayIcon_ = new QSystemTrayIcon(QIcon(QStringLiteral(":/icon.ico")),
                                          this);
    this->trayIcon_->setToolTip(QStringLiteral("Leafyrino"));

    this->menu_ = new QMenu;
    this->openAction_ = this->menu_->addAction(QStringLiteral("Open Leafyrino"));
    QObject::connect(this->openAction_, &QAction::triggered, this,
                     [this] {
                         this->restoreFromTray();
                     });

    this->openLastPingAction_ =
        this->menu_->addAction(QStringLiteral("Open last ping"));
    QObject::connect(this->openLastPingAction_, &QAction::triggered, this,
                     [this] {
                         this->openRecentPing(0);
                     });

    this->recentPingsMenu_ =
        this->menu_->addMenu(QStringLiteral("Recent pings"));
    this->menu_->addSeparator();

    this->mutePingsAction_ =
        this->menu_->addAction(QStringLiteral(
            "Mute ping notifications for 15 minutes"));
    QObject::connect(this->mutePingsAction_, &QAction::triggered, this,
                     [this] {
                         if (this->notificationsMuted())
                         {
                             this->mutePingsUntil_ = {};
                         }
                         else
                         {
                             this->mutePingsUntil_ =
                                 QDateTime::currentDateTimeUtc().addSecs(15 *
                                                                         60);
                         }
                         this->refreshMenu();
                     });

    this->pingNotificationsAction_ =
        this->menu_->addAction(QStringLiteral(
            "Show notifications for sound-enabled highlights"));
    this->pingNotificationsAction_->setCheckable(true);
    QObject::connect(this->pingNotificationsAction_, &QAction::toggled, this,
                     [](bool checked) {
                         getSettings()->trayNotifyOnSoundHighlights = checked;
                     });

    this->menu_->addSeparator();
    this->quitAction_ = this->menu_->addAction(QStringLiteral("Quit Leafyrino"));
    QObject::connect(this->quitAction_, &QAction::triggered, qApp, [] {
        QApplication::exit(0);
    });

    QObject::connect(this->menu_, &QMenu::aboutToShow, this, [this] {
        this->refreshMenu();
    });

    this->trayIcon_->setContextMenu(this->menu_);

    QObject::connect(
        this->trayIcon_, &QSystemTrayIcon::activated, this,
        [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger ||
                reason == QSystemTrayIcon::DoubleClick)
            {
                this->restoreFromTray();
            }
        });

    QObject::connect(this->trayIcon_, &QSystemTrayIcon::messageClicked, this,
                     [this] {
                         this->openRecentPing(0);
                     });

    this->refreshMenu();
    this->updateTrayVisibility();
}

void TrayController::updateTrayVisibility()
{
    if (this->trayIcon_ == nullptr)
    {
        return;
    }

    const bool shouldShow =
        getSettings()->trayHideOnClose || this->hiddenToTray_;
    this->trayIcon_->setVisible(shouldShow &&
                                QSystemTrayIcon::isSystemTrayAvailable());
}

void TrayController::refreshMenu()
{
    if (this->menu_ == nullptr)
    {
        return;
    }

    this->openLastPingAction_->setEnabled(!this->recentPings_.empty());
    this->rebuildRecentPingMenu();

    const bool canNotify =
        Toasts::isHighlightNotificationSupported() ||
        (this->trayIcon_ != nullptr && QSystemTrayIcon::supportsMessages());
    this->mutePingsAction_->setEnabled(canNotify);
    this->pingNotificationsAction_->setEnabled(canNotify);

    if (this->notificationsMuted())
    {
        this->mutePingsAction_->setText(QStringLiteral(
            "Unmute ping notifications"));
    }
    else
    {
        this->mutePingsAction_->setText(QStringLiteral(
            "Mute ping notifications for 15 minutes"));
    }

    {
        QSignalBlocker blocker(this->pingNotificationsAction_);
        this->pingNotificationsAction_->setChecked(
            getSettings()->trayNotifyOnSoundHighlights);
    }
}

void TrayController::rebuildRecentPingMenu()
{
    if (this->recentPingsMenu_ == nullptr)
    {
        return;
    }

    this->recentPingsMenu_->clear();
    this->recentPingsMenu_->setEnabled(!this->recentPings_.empty());

    if (this->recentPings_.empty())
    {
        auto *action =
            this->recentPingsMenu_->addAction(QStringLiteral("No recent pings"));
        action->setEnabled(false);
        return;
    }

    for (int i = 0; i < static_cast<int>(this->recentPings_.size()); ++i)
    {
        const auto ping = this->recentPings_[static_cast<size_t>(i)];
        auto *action = this->recentPingsMenu_->addAction(
            this->formatPingActionText(ping));
        QObject::connect(action, &QAction::triggered, this, [this, i] {
            this->openRecentPing(i);
        });
    }
}

void TrayController::showTrayPingNotification(const RecentPing &ping)
{
    const auto title = ping.displayName.isEmpty()
                           ? QStringLiteral("Highlight in #%1")
                                 .arg(ping.channelName)
                           : QStringLiteral("%1 in #%2")
                                 .arg(ping.displayName, ping.channelName);
    const auto body =
        !ping.messageText.isEmpty()
            ? ping.messageText
            : QStringLiteral("You were highlighted in #%1").arg(
                  ping.channelName);

    bool shown = getApp()->getToasts()->sendHighlightNotification(
        ping.channelName, title, body, ping.messageId);

    if (!shown && this->trayIcon_ != nullptr &&
        QSystemTrayIcon::supportsMessages())
    {
        this->trayIcon_->showMessage(title, body, QSystemTrayIcon::Information,
                                     10000);
    }
}

bool TrayController::notificationsMuted() const
{
    return this->mutePingsUntil_.isValid() &&
           QDateTime::currentDateTimeUtc() < this->mutePingsUntil_;
}

QString TrayController::formatPingActionText(const RecentPing &ping) const
{
    auto text = QStringLiteral("#%1").arg(ping.channelName);
    if (!ping.displayName.isEmpty())
    {
        text += QStringLiteral(" - %1").arg(ping.displayName);
    }
    if (!ping.messageText.isEmpty())
    {
        text += QStringLiteral(": %1").arg(ping.messageText);
    }
    return cleanForTray(text);
}

}

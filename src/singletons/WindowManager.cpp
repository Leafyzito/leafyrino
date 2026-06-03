// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/WindowManager.hpp"

#include "Application.hpp"
#include "common/Args.hpp"
#include "common/Channel.hpp"
#include "common/QLogging.hpp"
#ifndef Q_OS_MACOS
#    include "controllers/tray/TrayController.hpp"
#endif
#include "debug/AssertInGuiThread.hpp"
#include "messages/MessageElement.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "util/CombinePath.hpp"
#include "util/FilesystemHelpers.hpp"
#include "util/MultiChannel.hpp"
#include "util/SignalListener.hpp"
#include "widgets/AccountSwitchPopup.hpp"
#include "widgets/dialogs/SettingsDialog.hpp"
#include "widgets/FramelessEmbedWindow.hpp"
#include "widgets/helper/NotebookTab.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/OverlayWindow.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#include <pajlada/settings/backup.hpp>
#include <QApplication>
#include <QColor>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPointer>
#include <QSaveFile>
#include <QScreen>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <optional>

namespace {

std::optional<bool> &shouldMoveOutOfBoundsWindow()
{
    static std::optional<bool> x;
    return x;
}

void closeWindowsRecursive(QWidget *window)
{
    if (window->isWindow() && window->isVisible())
    {
        window->close();
    }

    for (auto *child : window->children())
    {
        if (child->isWidgetType())
        {
            // We check if it's a widget above
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
            closeWindowsRecursive(static_cast<QWidget *>(child));
        }
    }
}

bool isManagedWindow(QWidget *widget,
                     const std::vector<chatterino::Window *> &windows)
{
    return std::ranges::any_of(windows, [widget](chatterino::Window *window) {
        return window == widget;
    });
}

void closeTransientTopLevelWidgets(
    const std::vector<chatterino::Window *> &windows)
{
    std::vector<QPointer<QWidget>> widgetsToClose;

    for (auto *widget : QApplication::topLevelWidgets())
    {
        if (widget == nullptr || !widget->isVisible() ||
            isManagedWindow(widget, windows))
        {
            continue;
        }

        widgetsToClose.emplace_back(widget);
    }

    for (const auto &widget : widgetsToClose)
    {
        if (widget != nullptr)
        {
            widget->close();
        }
    }
}

}  // namespace

namespace chatterino {

const QString WindowManager::WINDOW_LAYOUT_FILENAME(
    QStringLiteral("window-layout.json"));

using SplitNode = SplitContainer::Node;

void WindowManager::showSettingsDialog(QWidget *parent,
                                       SettingsDialogPreference preference)
{
    if (this->appArgs.dontSaveSettings)
    {
        QMessageBox::critical(parent, "Chatterino - Editing Settings Forbidden",
                              "Settings cannot be edited when running with\n"
                              "commandline arguments such as '-c'.");
    }
    else
    {
        QTimer::singleShot(80, [parent, preference] {
            SettingsDialog::showDialog(parent, preference);
        });
    }
}

void WindowManager::showAccountSelectPopup(QPoint point)
{
    static auto *w = new AccountSwitchPopup;

    if (w->hasFocus())
    {
        w->hide();
        return;
    }

    w->refresh();

    w->moveTo(point - QPoint(30, 0), widgets::BoundsChecking::CursorPosition);
    w->show();
    w->setFocus();
}

WindowManager::WindowManager(const Args &appArgs_, const Paths &paths,
                             Settings &settings, Theme &themes_, Fonts &fonts)
    : themes(themes_)
    , appArgs(appArgs_)
    , windowLayoutFilePath(combinePath(paths.settingsDirectory,
                                       WindowManager::WINDOW_LAYOUT_FILENAME))
    , updateWordTypeMaskListener([this] {
        this->updateWordTypeMask();
    })
    , forceLayoutChannelViewsListener([this] {
        this->forceLayoutChannelViews();
    })
    , layoutChannelViewsListener([this] {
        this->layoutChannelViews();
    })
    , invalidateChannelViewBuffersListener([this] {
        this->invalidateChannelViewBuffers();
    })
    , repaintVisibleChatWidgetsListener([this] {
        this->repaintVisibleChatWidgets();
    })
{
    qCDebug(chatterinoWindowmanager) << "init WindowManager";

    this->updateWordTypeMaskListener.add(settings.showTimestamps);
    this->updateWordTypeMaskListener.add(settings.showBadgesGlobalAuthority);
    this->updateWordTypeMaskListener.add(settings.showBadgesPredictions);
    this->updateWordTypeMaskListener.add(settings.showBadgesChannelAuthority);
    this->updateWordTypeMaskListener.add(settings.showBadgesSubscription);
    this->updateWordTypeMaskListener.add(settings.showBadgesVanity);
    this->updateWordTypeMaskListener.add(settings.showBadgesChatterino);
    this->updateWordTypeMaskListener.add(settings.showBadgesFfz);
    this->updateWordTypeMaskListener.add(settings.showBadgesBttv);
    this->updateWordTypeMaskListener.add(settings.showBadgesSevenTV);
    this->updateWordTypeMaskListener.add(settings.showBadgesHomiesSupporter);
    this->updateWordTypeMaskListener.add(settings.showBadgesHomiesCustom);
    this->updateWordTypeMaskListener.add(settings.showBadgesMoltorino);
    this->updateWordTypeMaskListener.add(settings.showBadgesFolhinha);
    this->updateWordTypeMaskListener.add(settings.enableEmoteImages);
    this->updateWordTypeMaskListener.add(settings.lowercaseDomains);
    this->updateWordTypeMaskListener.add(settings.showReplyButton);

    this->forceLayoutChannelViewsListener.add(
        settings.moderationActions.delayedItemsChanged);
    this->forceLayoutChannelViewsListener.add(
        settings.highlightedMessages.delayedItemsChanged);
    this->forceLayoutChannelViewsListener.add(
        settings.highlightedUsers.delayedItemsChanged);
    this->forceLayoutChannelViewsListener.add(
        settings.highlightedBadges.delayedItemsChanged);
    this->forceLayoutChannelViewsListener.add(
        settings.removeSpacesBetweenEmotes);
    this->forceLayoutChannelViewsListener.add(settings.emoteScale);
    this->forceLayoutChannelViewsListener.add(settings.timestampFormat);
    this->forceLayoutChannelViewsListener.add(settings.collpseMessagesMinLines);
    this->forceLayoutChannelViewsListener.add(settings.enableRedeemedHighlight);
    this->forceLayoutChannelViewsListener.add(
        settings.showPinButtonOnModeratorsMode);
    this->forceLayoutChannelViewsListener.add(settings.showSelfDeleteButton);
    this->forceLayoutChannelViewsListener.add(
        settings.enableRepeatedMessageDetector);
    this->forceLayoutChannelViewsListener.add(
        settings.repeatedMessagesShowOnlyModerationMode);
    this->forceLayoutChannelViewsListener.add(
        settings.repeatedMessagesShowInUsercards);
    this->forceLayoutChannelViewsListener.add(settings.colorUsernames);
    this->forceLayoutChannelViewsListener.add(settings.boldUsernames);
    this->forceLayoutChannelViewsListener.add(
        settings.showBlockedTermAutomodMessages);
    this->forceLayoutChannelViewsListener.add(settings.hideModerated);
    this->forceLayoutChannelViewsListener.add(
        settings.streamerModeHideModActions);
    this->forceLayoutChannelViewsListener.add(
        settings.streamerModeHideRestrictedUsers);
    this->forceLayoutChannelViewsListener.add(fonts.fontChanged);

    this->layoutChannelViewsListener.add(settings.timestampFormat);

    this->invalidateChannelViewBuffersListener.add(settings.alternateMessages);
    this->invalidateChannelViewBuffersListener.add(settings.separateMessages);
    this->invalidateChannelViewBuffersListener.add(settings.fadeMessageHistory);
    this->invalidateChannelViewBuffersListener.add(
        settings.showClientDetectionHighlights);
    this->invalidateChannelViewBuffersListener.add(
        settings.clientDetectionWebColor);
    this->invalidateChannelViewBuffersListener.add(
        settings.clientDetectionAndroidColor);
    this->invalidateChannelViewBuffersListener.add(
        settings.clientDetectionIosColor);

    this->repaintVisibleChatWidgetsListener.add(
        this->themes.repaintVisibleChatWidgets_);

    this->saveTimer = new QTimer;

    this->saveTimer->setSingleShot(true);

    QObject::connect(this->saveTimer, &QTimer::timeout, [] {
        getApp()->getWindows()->save();
    });

#ifndef Q_OS_MACOS
    this->trayController_ = std::make_unique<TrayController>(*this);
#endif

    this->updateWordTypeMask();
}

WindowManager::~WindowManager() = default;

MessageElementFlags WindowManager::getWordFlags()
{
    return this->wordFlags_;
}

void WindowManager::updateWordTypeMask()
{
    using MEF = MessageElementFlag;
    auto *settings = getSettings();

    // text
    auto flags = MessageElementFlags(MEF::Text);

    // timestamp
    if (settings->showTimestamps)
    {
        flags.set(MEF::Timestamp);
    }

    // emotes
    if (settings->enableEmoteImages)
    {
        flags.set(MEF::EmoteImage);
    }
    flags.set(MEF::EmoteText);
    flags.set(MEF::EmojiText);

    // bits
    flags.set(MEF::BitsAmount);
    flags.set(settings->animateEmotes ? MEF::BitsAnimated : MEF::BitsStatic);

    // badges
    flags.set(MEF::BadgeSharedChannel);
    flags.set(settings->showBadgesGlobalAuthority ? MEF::BadgeGlobalAuthority
                                                  : MEF::None);
    flags.set(settings->showBadgesPredictions ? MEF::BadgePredictions
                                              : MEF::None);
    flags.set(settings->showBadgesChannelAuthority ? MEF::BadgeChannelAuthority
                                                   : MEF::None);
    flags.set(settings->showBadgesSubscription ? MEF::BadgeSubscription
                                               : MEF::None);
    flags.set(settings->showBadgesVanity ? MEF::BadgeVanity : MEF::None);
    flags.set(settings->showBadgesChatterino ? MEF::BadgeChatterino
                                             : MEF::None);
    flags.set(settings->showBadgesFfz ? MEF::BadgeFfz : MEF::None);
    flags.set(settings->showBadgesBttv ? MEF::BadgeBttv : MEF::None);
    flags.set(settings->showBadgesSevenTV ? MEF::BadgeSevenTV : MEF::None);
    flags.set(settings->showBadgesHomiesSupporter.getValue()
                  ? MEF::BadgeHomiesSupporter
                  : MEF::None);
    flags.set(settings->showBadgesHomiesCustom.getValue()
                  ? MEF::BadgeHomiesCustom
                  : MEF::None);
    flags.set(settings->showBadgesMoltorino ? MEF::BadgeMoltorino
                                            : MEF::None);
    flags.set(settings->showBadgesFolhinha ? MEF::BadgeFolhinha : MEF::None);

    // username
    flags.set(MEF::Username);

    // replies
    flags.set(MEF::RepliedMessage);
    flags.set(settings->showReplyButton ? MEF::ReplyButton : MEF::None);

    // misc
    flags.set(MEF::AlwaysShow);
    flags.set(MEF::Collapsed);
    flags.set(MEF::LowercaseLinks, settings->lowercaseDomains);
    flags.set(MEF::ChannelPointReward);

    // update flags
    MessageElementFlags newFlags = static_cast<MessageElementFlags>(flags);

    if (newFlags != this->wordFlags_)
    {
        this->wordFlags_ = newFlags;

        this->wordFlagsChanged.invoke();
    }
}

void WindowManager::layoutChannelViews(Channel *channel)
{
    this->layoutRequested.invoke(channel);
}

void WindowManager::forceLayoutChannelViews()
{
    this->incGeneration();
    this->layoutChannelViews(nullptr);
}

void WindowManager::invalidateChannelViewBuffers(Channel *channel)
{
    this->invalidateBuffersRequested.invoke(channel);
}

void WindowManager::repaintVisibleChatWidgets(Channel *channel)
{
    this->layoutRequested.invoke(channel);
}

void WindowManager::repaintGifEmotes()
{
    this->gifRepaintRequested.invoke();
}

// void WindowManager::updateAll()
//{
//    if (this->mainWindow != nullptr) {
//        this->mainWindow->update();
//    }
//}

Window &WindowManager::getMainWindow()
{
    assertInGuiThread();

    return *this->mainWindow_;
}

Window *WindowManager::getLastSelectedWindow() const
{
    assertInGuiThread();
    if (this->selectedWindow_ == nullptr)
    {
        return this->mainWindow_;
    }

    return this->selectedWindow_;
}

Window &WindowManager::createWindow(WindowType type, bool show, QWidget *parent)
{
    assertInGuiThread();

    auto *const realParent = [this, type, parent]() -> QWidget * {
        (void)this;
        (void)type;

        if (parent)
        {
            // If a parent is explicitly specified, we use that immediately.
            return parent;
        }

        // FIXME: On Windows, parenting popup windows causes unwanted behavior (see
        //        https://github.com/Chatterino/chatterino2/issues/4179 for discussion). Ideally, we
        //        would use a different solution rather than relying on OS-specific code but this is
        //        the low-effort fix for now.
#ifndef Q_OS_WIN
        if (type == WindowType::Popup)
        {
            // On some window managers, popup windows require a parent to behave correctly. See
            // https://github.com/Chatterino/chatterino2/pull/1843 for additional context.
            return &(this->getMainWindow());
        }
#endif

        // If no parent is set and something other than a popup window is being created, we fall
        // back to the default behavior of no parent.
        return nullptr;
    }();

    auto *window = new Window(type, realParent);

    this->windows_.push_back(window);
    if (show)
    {
        window->show();
    }

    if (type != WindowType::Main)
    {
        window->setAttribute(Qt::WA_DeleteOnClose);

        QObject::connect(window, &QWidget::destroyed, this, [this, window] {
            for (auto it = this->windows_.begin(); it != this->windows_.end();
                 it++)
            {
                if (*it == window)
                {
                    this->windows_.erase(it);
                    break;
                }
            }
        });
    }

    return *window;
}

Window &WindowManager::openInPopup(ChannelPtr channel)
{
    auto &popup = this->createWindow(WindowType::Popup, true);
    auto *split =
        popup.getNotebook().getOrAddSelectedPage()->appendNewSplit(false);
    split->setChannel(channel);

    return popup;
}

void WindowManager::select(Split *split)
{
    this->selectSplit.invoke(split);
}

void WindowManager::select(SplitContainer *container)
{
    this->selectSplitContainer.invoke(container);
}

void WindowManager::scrollToMessage(const MessagePtr &message)
{
    this->scrollToMessageSignal.invoke(message);
}

void WindowManager::openChannelOrMessageFromTray(const QString &channelName,
                                                 const QString &messageId)
{
    assertInGuiThread();

    auto normalizedChannel = channelName.trimmed();
    if (normalizedChannel.startsWith('#'))
    {
        normalizedChannel.remove(0, 1);
    }
    if (normalizedChannel.isEmpty())
    {
        this->showMainWindow();
        return;
    }

    this->showMainWindow();

    MessagePtr targetMessage;
    auto channel = getApp()->getTwitch()->getChannelOrEmpty(normalizedChannel);
    if (!messageId.isEmpty() && channel != nullptr && !channel->isEmpty())
    {
        targetMessage = channel->findMessageByID(messageId);
    }

    auto scrollToTargetMessage = [this, targetMessage] {
        if (targetMessage != nullptr)
        {
            QTimer::singleShot(0, this, [this, targetMessage] {
                this->scrollToMessage(targetMessage);
            });
        }
    };

    for (auto *window : this->windows_)
    {
        if (window == nullptr)
        {
            continue;
        }

        auto &notebook = window->getNotebook();
        for (int i = 0; i < notebook.getPageCount(); ++i)
        {
            auto *page = dynamic_cast<SplitContainer *>(notebook.getPageAt(i));
            if (page == nullptr)
            {
                continue;
            }

            for (auto *split : page->getSplits())
            {
                if (split == nullptr || split->getChannel() == nullptr)
                {
                    continue;
                }

                if (split->getChannel()->getName().compare(
                        normalizedChannel, Qt::CaseInsensitive) == 0)
                {
                    if (window->isMinimized())
                    {
                        window->showNormal();
                    }
                    else
                    {
                        window->show();
                    }
                    window->raise();
                    window->activateWindow();
                    this->selectedWindow_ = window;
                    notebook.select(page);
                    page->setSelected(split);
                    split->setFocus();
                    scrollToTargetMessage();
                    return;
                }
            }
        }
    }

    auto &mainWindow = this->getMainWindow();
    auto *page = mainWindow.getNotebook().getOrAddSelectedPage();
    if (page == nullptr)
    {
        scrollToTargetMessage();
        return;
    }

    auto *split = page->appendNewSplit(false);
    if (split == nullptr)
    {
        scrollToTargetMessage();
        return;
    }

    split->setChannel(getApp()->getTwitch()->getOrAddChannel(
        normalizedChannel));
    mainWindow.getNotebook().select(page);
    page->setSelected(split);
    split->setFocus();
    scrollToTargetMessage();
}

void WindowManager::showMainWindow()
{
    assertInGuiThread();

    if (this->mainWindow_ == nullptr)
    {
        return;
    }

    auto windowsToRestore = this->trayHiddenWindows_;
    this->trayHiddenWindows_.clear();
    for (auto *window : windowsToRestore)
    {
        if (window == nullptr)
        {
            continue;
        }

        if (window->isMinimized())
        {
            window->showNormal();
        }
        else
        {
            window->show();
        }
    }

    if (this->mainWindow_->isMinimized())
    {
        this->mainWindow_->showNormal();
    }
    else
    {
        this->mainWindow_->show();
    }

    this->mainWindow_->raise();
    this->mainWindow_->activateWindow();
    this->selectedWindow_ = this->mainWindow_;

#ifndef Q_OS_MACOS
    if (this->trayController_ != nullptr)
    {
        this->trayController_->markWindowShown();
    }
#endif
}

bool WindowManager::hideMainWindowToTray()
{
#ifdef Q_OS_MACOS
    return false;
#else
    assertInGuiThread();

    if (this->shuttingDown_ || isAppAboutToQuit())
    {
        return false;
    }

#ifndef QT_NO_SESSIONMANAGER
    if (qApp != nullptr && qApp->isSavingSession())
    {
        qCDebug(chatterinoWindowmanager)
            << "Skipping tray hide during session shutdown";
        return false;
    }
#endif

    if (this->trayController_ == nullptr ||
        !this->trayController_->canHideToTray())
    {
        return false;
    }

    this->save();
    closeTransientTopLevelWidgets(this->windows_);
    this->trayController_->hideToTray();

    this->trayHiddenWindows_.clear();
    for (auto *window : this->windows_)
    {
        if (window != nullptr && window->isVisible())
        {
            this->trayHiddenWindows_.push_back(window);
            window->hide();
        }
    }

    this->selectedWindow_ = nullptr;
    return true;
#endif
}

void WindowManager::notifyTrayHighlight(const Channel *channel,
                                        const MessagePtr &message,
                                        bool playSound)
{
#ifdef Q_OS_MACOS
    (void)channel;
    (void)message;
    (void)playSound;
#else
    if (this->trayController_ != nullptr)
    {
        this->trayController_->notifyHighlight(channel, message, playSound);
    }
#endif
}

QRect WindowManager::emotePopupBounds() const
{
    return this->emotePopupBounds_;
}

void WindowManager::setEmotePopupBounds(QRect bounds)
{
    if (this->emotePopupBounds_ != bounds)
    {
        this->emotePopupBounds_ = bounds;
        this->queueSave();
    }
}

void WindowManager::initialize()
{
    assertInGuiThread();

    {
        WindowLayout windowLayout;

        if (this->appArgs.customChannelLayout)
        {
            windowLayout = this->appArgs.customChannelLayout.value();
        }
        else
        {
            windowLayout = this->loadWindowLayoutFromFile();
        }

        auto desired = this->appArgs.activateChannel;
        if (desired)
        {
            windowLayout.activateOrAddChannel(desired->provider, desired->name);
        }

        this->emotePopupBounds_ = windowLayout.emotePopupBounds_;

        this->applyWindowLayout(windowLayout);
    }

    if (this->appArgs.isFramelessEmbed)
    {
        this->framelessEmbedWindow_.reset(new FramelessEmbedWindow);
        this->framelessEmbedWindow_->show();
    }

    // No main window has been created from loading, create an empty one
    if (this->mainWindow_ == nullptr)
    {
        this->mainWindow_ = &this->createWindow(WindowType::Main);
        this->mainWindow_->getNotebook().addPage(true);

        // TODO: don't create main window if it's a frameless embed
        if (this->appArgs.isFramelessEmbed)
        {
            this->mainWindow_->hide();
        }
    }
}

void WindowManager::save()
{
    if (this->appArgs.dontSaveSettings)
    {
        return;
    }

    if (this->shuttingDown_)
    {
        qCDebug(chatterinoWindowmanager) << "Skipping save (shutting down)";
        return;
    }

    qCDebug(chatterinoWindowmanager) << "Saving";
    assertInGuiThread();

    if (this->windows_.empty())
    {
        qCWarning(chatterinoWindowmanager)
            << "Skipping window layout save because no windows are managed";
        return;
    }

    QJsonDocument document;

    // "serialize"
    QJsonArray windowArr;
    for (Window *window : this->windows_)
    {
        QJsonObject windowObj;

        // window type
        switch (window->getType())
        {
            case WindowType::Main:
                windowObj.insert("type", "main");
                break;

            case WindowType::Popup:
                windowObj.insert("type", "popup");
                break;

            case WindowType::Attached:;
        }

        if (window->isMaximized())
        {
            windowObj.insert("state", "maximized");
        }
        else if (window->isMinimized())
        {
            windowObj.insert("state", "minimized");
        }

        // window geometry
        auto rect = window->getBounds();

        windowObj.insert("x", rect.x());
        windowObj.insert("y", rect.y());
        windowObj.insert("width", rect.width());
        windowObj.insert("height", rect.height());

        windowObj["emotePopup"] = QJsonObject{
            {"x", this->emotePopupBounds_.x()},
            {"y", this->emotePopupBounds_.y()},
            {"width", this->emotePopupBounds_.width()},
            {"height", this->emotePopupBounds_.height()},
        };

        // window tabs
        QJsonArray tabsArr;

        for (int tabIndex = 0; tabIndex < window->getNotebook().getPageCount();
             tabIndex++)
        {
            QJsonObject tabObj;
            SplitContainer *tab = dynamic_cast<SplitContainer *>(
                window->getNotebook().getPageAt(tabIndex));
            assert(tab != nullptr);

            bool isSelected = window->getNotebook().getSelectedPage() == tab;
            WindowManager::encodeTab(tab, isSelected, tabObj);
            tabsArr.append(tabObj);
        }

        windowObj.insert("tabs", tabsArr);
        windowArr.append(windowObj);
    }

    QJsonObject obj;
    obj.insert("windows", windowArr);
    document.setObject(obj);

    // save file
    std::error_code ec;
    pajlada::Settings::Backup::saveWithBackup(
        qStringToStdPath(this->windowLayoutFilePath),
        {.enabled = true, .numSlots = 9},
        [&](const auto &path, auto &ec) {
            QSaveFile file(stdPathToQString(path));
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            {
                ec = std::make_error_code(std::errc::io_error);
                return;
            }

            file.write(document.toJson(QJsonDocument::Indented));
            if (!file.commit() || file.error() != QFile::NoError)
            {
                ec = std::make_error_code(std::errc::io_error);
            }
        },
        ec);

    if (ec)
    {
        // TODO(Qt 6.5): drop fromStdString
        qCWarning(chatterinoWindowmanager)
            << "Failed to save windowlayout"
            << QString::fromStdString(ec.message());
    }
}

void WindowManager::sendAlert()
{
    int flashDuration = 2500;
    if (getSettings()->longAlerts)
    {
        flashDuration = 0;
    }
    QApplication::alert(this->getMainWindow().window(), flashDuration);
}

void WindowManager::queueSave()
{
    using namespace std::chrono_literals;

    this->saveTimer->start(10s);
}

void WindowManager::toggleAllOverlayInertia()
{
    // check if any window is not inert
    bool anyNonInert = false;
    for (auto *window : this->windows_)
    {
        if (anyNonInert)
        {
            break;
        }
        window->getNotebook().forEachSplit([&](auto *split) {
            auto *overlay = split->overlayWindow();
            if (overlay)
            {
                anyNonInert = anyNonInert || !overlay->isInert();
            }
        });
    }

    for (auto *window : this->windows_)
    {
        window->getNotebook().forEachSplit([&](auto *split) {
            auto *overlay = split->overlayWindow();
            if (overlay)
            {
                overlay->setInert(anyNonInert);
            }
        });
    }
}

std::set<QString> WindowManager::getVisibleChannelNames() const
{
    std::set<QString> visible;
    for (auto *window : this->windows_)
    {
        auto *page = window->getNotebook().getSelectedPage();
        if (!page)
        {
            continue;
        }

        for (auto *split : page->getSplits())
        {
            visible.emplace(split->getChannel()->getName());
        }
    }

    return visible;
}

QJsonArray WindowManager::getOpenTabSnapshot() const
{
    QJsonArray windowsArray;

    for (auto *window : this->windows_)
    {
        QJsonObject windowObj;
        windowObj["windowType"] = static_cast<int>(window->getType());
        windowObj["activeWindow"] = window->isActiveWindow();

        QJsonArray tabsArray;
        auto &notebook = window->getNotebook();
        auto *selectedPage = notebook.getSelectedPage();

        for (int tabIndex = 0; tabIndex < notebook.getPageCount(); ++tabIndex)
        {
            auto *page =
                dynamic_cast<SplitContainer *>(notebook.getPageAt(tabIndex));
            if (page == nullptr)
            {
                continue;
            }

            QJsonObject tabObj;
            tabObj["index"] = tabIndex;
            tabObj["selected"] = selectedPage == page;
            if (auto *tab = page->getTab(); tab != nullptr && tab->hasCustomTitle())
            {
                tabObj["customTitle"] = tab->getCustomTitle();
            }

            QJsonArray channelsArray;
            for (auto *split : page->getSplits())
            {
                if (auto channel = split->getChannel())
                {
                    channelsArray.append(channel->getName());
                }
            }
            tabObj["channels"] = channelsArray;

            if (auto *selectedSplit = page->getSelectedSplit())
            {
                if (auto selectedChannel = selectedSplit->getChannel())
                {
                    tabObj["selectedChannel"] = selectedChannel->getName();
                }
            }

            tabsArray.append(tabObj);
        }

        windowObj["tabs"] = tabsArray;
        windowsArray.append(windowObj);
    }

    return windowsArray;
}

void WindowManager::encodeTab(SplitContainer *tab, bool isSelected,
                              QJsonObject &obj)
{
    // custom tab title
    if (tab->getTab()->hasCustomTitle())
    {
        obj.insert("title", tab->getTab()->getCustomTitle());
    }

    // custom tab color
    if (tab->getTab()->hasCustomTabColor())
    {
        obj.insert("tabColor",
                   tab->getTab()->getCustomTabColor().name(QColor::HexArgb));
    }

    // selected
    if (isSelected)
    {
        obj.insert("selected", true);
    }

    // highlighting on new messages
    obj.insert("highlightsEnabled", tab->getTab()->hasHighlightsEnabled());

    // splits
    QJsonObject splits;

    WindowManager::encodeNodeRecursively(tab->getBaseNode(), splits);

    obj.insert("splits2", splits);
}

void WindowManager::encodeNodeRecursively(SplitNode *node, QJsonObject &obj)
{
    switch (node->getType())
    {
        case SplitNode::Type::Split: {
            obj.insert("type", "split");
            obj.insert("moderationMode", node->getSplit()->getModerationMode());

            QJsonObject split;
            WindowManager::encodeChannel(node->getSplit()->getIndirectChannel(),
                                         split);
            obj.insert("data", split);

            QJsonArray filters;
            WindowManager::encodeFilters(node->getSplit(), filters);
            obj.insert("filters", filters);

            auto spellOverride = node->getSplit()->checkSpellingOverride();
            if (spellOverride)
            {
                obj["checkSpelling"] = *spellOverride;
            }

            if (node->getSplit()->perSplitHidePinnedMessage())
            {
                obj.insert(QStringLiteral("splitHidePinnedMessage"), true);
            }
            if (node->getSplit()->perSplitHidePrediction())
            {
                obj.insert(QStringLiteral("splitHidePrediction"), true);
            }
            if (node->getSplit()->perSplitHidePoll())
            {
                obj.insert(QStringLiteral("splitHidePoll"), true);
            }
        }
        break;
        case SplitNode::Type::HorizontalContainer:
        case SplitNode::Type::VerticalContainer: {
            obj.insert("type",
                       node->getType() == SplitNode::Type::HorizontalContainer
                           ? "horizontal"
                           : "vertical");

            QJsonArray itemsArr;
            for (const auto &n : node->getChildren())
            {
                QJsonObject subObj;
                WindowManager::encodeNodeRecursively(n.get(), subObj);
                itemsArr.append(subObj);
            }
            obj.insert("items", itemsArr);
        }
        break;

        default:
            break;
    }

    obj.insert("flexh", node->getHorizontalFlex());
    obj.insert("flexv", node->getVerticalFlex());
}

void WindowManager::encodeChannel(IndirectChannel channel, QJsonObject &obj)
{
    assertInGuiThread();

    switch (channel.getType())
    {
        case Channel::Type::Twitch: {
            obj.insert("type", "twitch");
            obj.insert("name", channel.get()->getName());
            if (auto *twitchChannel =
                    dynamic_cast<TwitchChannel *>(channel.get().get()))
            {
                if (twitchChannel->isAnonymous())
                {
                    obj.insert("anonymous", true);
                }
            }
        }
        break;
        case Channel::Type::TwitchAutomod: {
            obj.insert("type", "automod");
        }
        break;
        case Channel::Type::TwitchMentions: {
            obj.insert("type", "mentions");
        }
        break;
        case Channel::Type::TwitchWatching: {
            obj.insert("type", "watching");
        }
        break;
        case Channel::Type::TwitchWhispers: {
            obj.insert("type", "whispers");
        }
        break;
        case Channel::Type::TwitchLive: {
            obj.insert("type", "live");
        }
        break;
        case Channel::Type::Misc: {
            obj.insert("type", "misc");
            obj.insert("name", channel.get()->getName());
        }
        break;
        case Channel::Type::Kick: {
            obj.insert("type", "kick");
            obj.insert("name", channel.get()->getName());
            auto *kc = dynamic_cast<KickChannel *>(channel.get().get());
            if (kc)
            {
                obj.insert("roomID", static_cast<qint64>(kc->roomID()));
                obj.insert("userID", static_cast<qint64>(kc->userID()));
                obj.insert("channelID", static_cast<qint64>(kc->channelID()));
            }
        }
        break;
        case Channel::Type::Multi: {
            obj.insert("type", "multi");
            auto *mc = dynamic_cast<MultiChannel *>(channel.get().get());
            if (mc)
            {
                QJsonArray children;
                for (const auto &child : mc->channels())
                {
                    children.append(child.descriptor().toJson());
                }
                obj.insert("children", children);
                obj.insert("indicatorMode",
                           qmagicenum::enumNameString(mc->indicatorMode()));
                obj.insert("activeIndex",
                           static_cast<int32_t>(mc->activeChannelIndex()));
            }
        }
        break;

        default:
            break;
    }
}

void WindowManager::encodeFilters(Split *split, QJsonArray &arr)
{
    assertInGuiThread();

    auto filters = split->getFilters();
    for (const auto &f : filters)
    {
        arr.append(f.toString(QUuid::WithoutBraces));
    }
}

IndirectChannel WindowManager::decodeChannel(const SplitDescriptor &descriptor)
{
    assertInGuiThread();

    if (descriptor.type_ == "twitch")
    {
        if (descriptor.anonymous_)
        {
            return getApp()->getTwitch()->getOrAddAnonymousChannel(
                descriptor.channelName_);
        }

        return getApp()->getTwitch()->getOrAddChannel(descriptor.channelName_);
    }
    else if (descriptor.type_ == "mentions")
    {
        return getApp()->getTwitch()->getMentionsChannel();
    }
    else if (descriptor.type_ == "watching")
    {
        return getApp()->getTwitch()->getWatchingChannel();
    }
    else if (descriptor.type_ == "whispers")
    {
        return getApp()->getTwitch()->getWhispersChannel();
    }
    else if (descriptor.type_ == "live")
    {
        return getApp()->getTwitch()->getLiveChannel();
    }
    else if (descriptor.type_ == "automod")
    {
        return getApp()->getTwitch()->getAutomodChannel();
    }
    else if (descriptor.type_ == "misc")
    {
        return getApp()->getTwitch()->getChannelOrEmpty(
            descriptor.channelName_);
    }
    else if (descriptor.type_ == "kick")
    {
        return getApp()->getKickChatServer()->getOrCreate(
            descriptor.channelName_, KickChannel::UserInit{
                                         .roomID = descriptor.kickRoomID,
                                         .userID = descriptor.kickUserID,
                                         .channelID = descriptor.kickChannelID,
                                     });
    }
    else if (descriptor.type_ == u"multi")
    {
        QVarLengthArray<MultiChannel::Spec, 4> specs;
        for (const auto &child : descriptor.children)
        {
            auto spec = MultiChannel::Spec::fromDescriptor(child);
            if (spec)
            {
                specs.emplace_back(*std::move(spec));
            }
        }
        auto ptr =
            std::make_shared<MultiChannel>(specs, descriptor.mcIndicator);
        ptr->setActiveChannelIndex(descriptor.mcIndex);
        return {std::move(ptr)};
    }

    return Channel::getEmpty();
}

void WindowManager::closeAll()
{
    assertInGuiThread();

    qCDebug(chatterinoWindowmanager) << "Shutting down (closing windows)";
    this->shuttingDown_ = true;

    for (Window *window : this->windows_)
    {
        closeWindowsRecursive(window);
    }
}

int WindowManager::getGeneration() const
{
    return this->generation_;
}

void WindowManager::incGeneration()
{
    this->generation_++;
}

WindowLayout WindowManager::loadWindowLayoutFromFile() const
{
    return WindowLayout::loadFromFile(this->windowLayoutFilePath);
}

void WindowManager::applyWindowLayout(const WindowLayout &layout)
{
    if (this->appArgs.dontLoadMainWindow)
    {
        return;
    }

    // Set emote popup position
    this->emotePopupBounds_ = layout.emotePopupBounds_;

    for (const auto &windowData : layout.windows_)
    {
        auto type = windowData.type_;

        Window &window = this->createWindow(type, false);

        if (type == WindowType::Main)
        {
            assert(this->mainWindow_ == nullptr);

            this->mainWindow_ = &window;
        }

        // get geometry
        {
            // out of bounds windows
            auto screens = QApplication::screens();
            bool outOfBounds =
                !qEnvironmentVariableIsSet("I3SOCK") &&
                std::none_of(screens.begin(), screens.end(),
                             [&](QScreen *screen) {
                                 return screen->availableGeometry().intersects(
                                     windowData.geometry_);
                             });

            // ask if move into bounds
            auto &&should = shouldMoveOutOfBoundsWindow();
            if (outOfBounds && !should)
            {
                should =
                    QMessageBox(QMessageBox::Icon::Warning,
                                "Windows out of bounds",
                                "Some windows were detected out of bounds. "
                                "Should they be moved into bounds?",
                                QMessageBox::Yes | QMessageBox::No)
                        .exec() == QMessageBox::Yes;
            }

            if ((!outOfBounds || !should.value()) &&
                windowData.geometry_.x() != -1 &&
                windowData.geometry_.y() != -1 &&
                windowData.geometry_.width() != -1 &&
                windowData.geometry_.height() != -1)
            {
                // Have to offset x by one because qt moves the window 1px too
                // far to the left:w

                window.setInitialBounds(
                    {
                        windowData.geometry_.x(),
                        windowData.geometry_.y(),
                        windowData.geometry_.width(),
                        windowData.geometry_.height(),
                    },
                    widgets::BoundsChecking::Off);
            }
        }

        // open tabs
        for (const auto &tab : windowData.tabs_)
        {
            SplitContainer *page = window.getNotebook().addPage(false);

            // set custom title
            if (!tab.customTitle_.isEmpty())
            {
                page->getTab()->setCustomTitle(tab.customTitle_);
            }

            // set custom tab color
            if (!tab.customTabColor_.isEmpty())
            {
                QColor color(tab.customTabColor_);
                if (color.isValid())
                {
                    page->getTab()->setCustomTabColor(color);
                }
            }

            // selected
            if (tab.selected_)
            {
                window.getNotebook().select(page);
            }

            // highlighting on new messages
            page->getTab()->setHighlightsEnabled(tab.highlightsEnabled_);

            if (tab.rootNode_)
            {
                page->applyFromDescriptor(*tab.rootNode_);
            }
        }
        window.show();

        // Set window state
        switch (windowData.state_)
        {
            case WindowDescriptor::State::Minimized: {
                window.setWindowState(Qt::WindowMinimized);
            }
            break;

            case WindowDescriptor::State::Maximized: {
                window.setWindowState(Qt::WindowMaximized);
            }
            break;

            case WindowDescriptor::State::None:
                break;
        }
    }
}

}  // namespace chatterino

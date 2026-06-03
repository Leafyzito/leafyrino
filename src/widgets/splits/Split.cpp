// SPDX-FileCopyrightText: 2016 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/Split.hpp"

#include "Application.hpp"
#include "common/Common.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/notifications/NotificationController.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchBadges.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/ImageUploader.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/CustomPlayer.hpp"
#include "util/MultiChannel.hpp"
#include "util/StreamLink.hpp"
#include "widgets/ChatterListWidget.hpp"
#include "widgets/dialogs/SelectChannelDialog.hpp"
#include "widgets/dialogs/SelectChannelFiltersDialog.hpp"
#include "widgets/dialogs/UserInfoPopup.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/DebugPopup.hpp"
#include "widgets/helper/MessageView.hpp"
#include "widgets/helper/NotebookTab.hpp"
#include "widgets/helper/PinnedMessageBanner.hpp"
#include "widgets/helper/PollBanner.hpp"
#include "widgets/helper/PredictionBanner.hpp"
#include "widgets/helper/ResizingTextEdit.hpp"
#include "widgets/helper/SearchPopup.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/OverlayWindow.hpp"
#include "widgets/Scrollbar.hpp"
#include "widgets/splits/DraggedSplit.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/splits/SplitHeader.hpp"
#include "widgets/splits/SplitInput.hpp"
#include "widgets/splits/SplitMpsOverlay.hpp"
#include "widgets/splits/SplitOverlay.hpp"
#include "widgets/Window.hpp"

#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDrag>
#include <QElapsedTimer>
#include <QEvent>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QMovie>
#include <QPainter>
#include <QSet>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>

#include <functional>
#include <optional>

namespace chatterino {
namespace {
constexpr int DEFERRED_TWITCH_FEATURE_REFRESH_DELAY_MS = 3500;
constexpr int DEFERRED_TWITCH_POLL_REFRESH_OFFSET_MS = 250;
constexpr int DEFERRED_TWITCH_POINTS_REFRESH_OFFSET_MS = 500;
constexpr int DEFERRED_TWITCH_WARNING_REFRESH_OFFSET_MS = 650;
constexpr int DEFERRED_TWITCH_CHATTERS_REFRESH_OFFSET_MS = 750;
constexpr int DEFERRED_TWITCH_ROOM_ID_RETRY_MS = 1000;
constexpr int DEFERRED_TWITCH_ROOM_ID_MAX_RETRIES = 12;
constexpr int INTERACTIVE_TWITCH_FEATURE_REFRESH_DELAY_MS = 75;
constexpr int INTERACTIVE_TWITCH_POLL_REFRESH_OFFSET_MS = 60;
constexpr int INTERACTIVE_TWITCH_POINTS_REFRESH_OFFSET_MS = 120;
constexpr int INTERACTIVE_TWITCH_WARNING_REFRESH_OFFSET_MS = 180;
constexpr int INTERACTIVE_TWITCH_CHATTERS_REFRESH_OFFSET_MS = 300;
constexpr int INTERACTIVE_TWITCH_ROOM_ID_RETRY_MS = 250;

QElapsedTimer &twitchStartupTimer()
{
    static QElapsedTimer timer = [] {
        QElapsedTimer created;
        created.start();
        return created;
    }();
    return timer;
}

bool shouldUseColdTwitchFeatureDelay()
{
    return twitchStartupTimer().elapsed() <
           DEFERRED_TWITCH_FEATURE_REFRESH_DELAY_MS;
}

void showTutorialVideo(QWidget *parent, const QString &source,
                       const QString &title, const QString &description)
{
    auto *window = new BasePopup(
        {
            BaseWindow::EnableCustomFrame,
            BaseWindow::BoundsCheckOnShow,
        },
        parent);
    window->setWindowTitle("Leafyrino - " + title);
    window->setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout();
    layout->addWidget(new QLabel(description));
    auto *label = new QLabel(window);
    layout->addWidget(label);
    auto *movie = new QMovie(label);
    movie->setFileName(source);
    label->setMovie(movie);
    movie->start();
    window->getLayoutContainer()->setLayout(layout);
    window->show();
}
}  // namespace

pajlada::Signals::Signal<Qt::KeyboardModifiers> Split::modifierStatusChanged;
Qt::KeyboardModifiers Split::modifierStatus = Qt::NoModifier;

Split::Split(QWidget *parent)
    : BaseWidget(parent)
    , channel_(Channel::getEmpty())
    , vbox_(new QVBoxLayout(this))
    , header_(new SplitHeader(this))
    , pinnedBanner_(new PinnedMessageBanner(this, this))
    , predictionBanner_(new PredictionBanner(this, this))
    , pollBanner_(new PollBanner(this, this))
    , view_(new ChannelView(this, this, ChannelView::Context::None,
                            getSettings()->scrollbackSplitLimit))
    , input_(new SplitInput(this))
    , overlay_(new SplitOverlay(this))
{
    this->setMouseTracking(true);
    this->view_->setPausable(true);
    this->view_->setFocusProxy(this->input_->ui_.textEdit);
    this->setFocusProxy(this->input_->ui_.textEdit);

    this->vbox_->setSpacing(0);
    this->vbox_->setContentsMargins(1, 1, 1, 1);

    this->vbox_->addWidget(this->header_);
    this->vbox_->addWidget(this->pinnedBanner_);
    this->vbox_->addWidget(this->predictionBanner_);
    this->vbox_->addWidget(this->pollBanner_);
    this->vbox_->addWidget(this->view_, 1);
    this->vbox_->addWidget(this->input_);

    this->input_->ui_.textEdit->installEventFilter(parent);

    // update placeholder text on Twitch account change and channel change
    this->bSignals_.emplace_back(
        getApp()->getAccounts()->twitch.currentUserChanged.connect([this] {
            this->updateInputPlaceholder();
            this->deferredTwitchForcePersonalRefresh_ = true;
            this->scheduleDeferredTwitchRefresh(true);
        }));
    this->signalHolder_.managedConnect(this->channelChanged, [this] {
        this->updateInputPlaceholder();
    });
    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->kick.currentUserChanged, [this] {
            this->updateInputPlaceholder();
        });
    this->updateInputPlaceholder();
    getSettings()->showInputPlaceholder.connect(
        [this](const bool &, auto) {
            this->updateInputPlaceholder();
        },
        this->signalHolder_);

    // clear SplitInput selection when selecting in ChannelView
    // this connection can be ignored since the ChannelView is owned by this Split
    std::ignore = this->view_->selectionChanged.connect([this]() {
        if (this->input_->hasSelection())
        {
            this->input_->clearSelection();
        }
    });

    // clear ChannelView selection when selecting in SplitInput
    // this connection can be ignored since the SplitInput is owned by this Split
    std::ignore = this->input_->selectionChanged.connect([this]() {
        if (this->view_->hasSelection())
        {
            this->view_->clearSelection();
        }
    });

    // this connection can be ignored since the ChannelView is owned by this Split
    std::ignore = this->view_->openChannelIn.connect(
        [this](QString twitchChannel, FromTwitchLinkOpenChannelIn openIn) {
            ChannelPtr channel =
                getApp()->getTwitch()->getOrAddChannel(twitchChannel);
            switch (openIn)
            {
                case FromTwitchLinkOpenChannelIn::Split:
                    this->openSplitRequested.invoke(channel);
                    break;
                case FromTwitchLinkOpenChannelIn::Tab:
                    this->joinChannelInNewTab(channel);
                    break;
                case FromTwitchLinkOpenChannelIn::BrowserPlayer:
                    this->openChannelInBrowserPlayer(channel);
                    break;
                case FromTwitchLinkOpenChannelIn::Streamlink:
                    this->openChannelInStreamlink(twitchChannel);
                    break;
                case FromTwitchLinkOpenChannelIn::CustomPlayer:
                    this->openChannelInCustomPlayer(twitchChannel);
                default:
                    qCWarning(chatterinoWidget)
                        << "Unhandled \"FromTwitchLinkOpenChannelIn\" enum "
                           "value: "
                        << static_cast<int>(openIn);
            }
        });

    // this connection can be ignored since the SplitInput is owned by this Split
    std::ignore =
        this->input_->textChanged.connect([this](const QString &newText) {
            if (getSettings()->showEmptyInput)
            {
                // We always show the input regardless of the text, so we can early out here
                return;
            }

            if (newText.isEmpty())
            {
                this->input_->hide();
            }
            else if (this->input_->isHidden())
            {
                // Text updated and the input was previously hidden, show it
                this->input_->show();
            }
        });

    getSettings()->showEmptyInput.connect(
        [this](const bool &showEmptyInput) {
            if (showEmptyInput)
            {
                this->input_->show();
            }
            else
            {
                if (this->input_->getInputText().isEmpty())
                {
                    this->input_->hide();
                }
            }
        },
        this->signalHolder_);

    this->header_->updateIcons();
    this->overlay_->hide();

    this->mpsOverlay_ = new SplitMpsOverlay(this);
    this->mpsOverlay_->setGeometry(this->rect());
    this->updateMpsOverlayAnchor();

    this->view_->installEventFilter(this);
    this->pinnedBanner_->installEventFilter(this);
    this->predictionBanner_->installEventFilter(this);
    this->pollBanner_->installEventFilter(this);
    this->installEventFilter(this);

    QTimer::singleShot(0, this, [this] {
        this->updateMpsOverlayAnchor();
    });

    QObject::connect(
        this->view_, &ChannelView::messageAddedToChannel, this,
        [this](MessagePtr &message) {
            if (getSettings()->pulseTextInputOnSelfMessage)
            {
                auto user = getApp()->getAccounts()->twitch.getCurrent();
                if (!user->isAnon() && message->userID == user->getUserId())
                {
                    this->input_->triggerSelfMessageReceived();
                }
            }

            if (this->mpsOverlay_)
            {
                this->mpsOverlay_->onMessageAdded();
            }
        });

    this->setSizePolicy(QSizePolicy::MinimumExpanding,
                        QSizePolicy::MinimumExpanding);

    // update moderation button when items changed
    this->signalHolder_.managedConnect(
        getSettings()->moderationActions.delayedItemsChanged, [this] {
            this->refreshModerationMode();
        });

    this->signalHolder_.managedConnect(modifierStatusChanged, [this](
                                                                  Qt::KeyboardModifiers
                                                                      status) {
        if ((status ==
             SHOW_SPLIT_OVERLAY_MODIFIERS /*|| status == showAddSplitRegions*/) &&
            this->isMouseOver_)
        {
            this->overlay_->show();
        }
        else
        {
            this->overlay_->hide();
        }

        if (getSettings()->pauseChatModifier.getEnum() != Qt::NoModifier &&
            status == getSettings()->pauseChatModifier.getEnum())
        {
            this->view_->pause(PauseReason::KeyboardModifier);
        }
        else
        {
            this->view_->unpause(PauseReason::KeyboardModifier);
        }
    });

    this->signalHolder_.managedConnect(this->input_->ui_.textEdit->focused,
                                       [this] {
                                           // Forward textEdit's focused event
                                           this->focused.invoke();
                                       });
    this->signalHolder_.managedConnect(this->focused, [this] {
        this->scheduleDeferredTwitchRefresh(true);
    });
    this->signalHolder_.managedConnect(this->input_->ui_.textEdit->focusLost,
                                       [this] {
                                           // Forward textEdit's focusLost event
                                           this->focusLost.invoke();
                                       });

    this->deferredTwitchRefreshTimer_ = new QTimer(this);
    this->deferredTwitchRefreshTimer_->setSingleShot(true);
    this->deferredTwitchRefreshTimer_->setInterval(
        DEFERRED_TWITCH_FEATURE_REFRESH_DELAY_MS);
    QObject::connect(this->deferredTwitchRefreshTimer_, &QTimer::timeout, this,
                     [this] {
                         this->runDeferredTwitchRefresh();
                     });

    // this connection can be ignored since the SplitInput is owned by this Split
    std::ignore = this->input_->ui_.textEdit->imagePasted.connect(
        [this](const QMimeData *original) {
            if (!getSettings()->imageUploaderEnabled)
            {
                return;
            }

            auto channel = this->getChannel();
            auto *imageUploader = getApp()->getImageUploader();

            auto [images, imageProcessError] =
                imageUploader->getImages(original);
            if (images.empty())
            {
                channel->addSystemMessage(
                    QString(
                        "An error occurred trying to process your image: %1")
                        .arg(imageProcessError));
                return;
            }

            if (getSettings()->askOnImageUpload.getValue())
            {
                QMessageBox msgBox(this->window());
                msgBox.setWindowTitle("Leafyrino");
                msgBox.setText("Image upload");
                msgBox.setInformativeText(
                    "You are uploading an image to a 3rd party service not in "
                    "control of the Leafyrino team. You may not be able to "
                    "remove the image from the site. Are you okay with this?");
                auto *cancel = msgBox.addButton(QMessageBox::Cancel);
                auto *yes = msgBox.addButton(QMessageBox::Yes);
                auto *yesDontAskAgain = msgBox.addButton("Yes, don't ask again",
                                                         QMessageBox::YesRole);

                msgBox.setDefaultButton(QMessageBox::Yes);

                msgBox.exec();

                auto *clickedButton = msgBox.clickedButton();
                if (clickedButton == yesDontAskAgain)
                {
                    getSettings()->askOnImageUpload.setValue(false);
                }
                else if (clickedButton == yes)
                {
                    // Continue with image upload
                }
                else if (clickedButton == cancel)
                {
                    // Not continuing with image upload
                    return;
                }
                else
                {
                    // An unknown "button" was pressed - handle it as if cancel was pressed
                    // cancel is already handled as the "escape" option, so this should never happen
                    qCWarning(chatterinoImageuploader)
                        << "Unhandled button pressed:" << clickedButton;
                    return;
                }
            }

            QPointer<ResizingTextEdit> edit = this->input_->ui_.textEdit;
            imageUploader->upload(std::move(images), channel, edit);
        });

    getSettings()->imageUploaderEnabled.connect(
        [this](const bool &val) {
            this->setAcceptDrops(val);
        },
        this->signalHolder_);
    this->addShortcuts();
    this->signalHolder_.managedConnect(getApp()->getHotkeys()->onItemsUpdated,
                                       [this]() {
                                           this->clearShortcuts();
                                           this->addShortcuts();
                                       });
}

void Split::addShortcuts()
{
    HotkeyController::HotkeyMap actions{
        {"delete",
         [this](const std::vector<QString> &) -> QString {
             this->deleteFromContainer();
             return "";
         }},
        {"changeChannel",
         [this](const std::vector<QString> &) -> QString {
             this->changeChannel();
             return "";
         }},
        {"showSearch",
         [this](const std::vector<QString> &) -> QString {
             this->showSearch(true);
             return "";
         }},
        {"showGlobalSearch",
         [this](const std::vector<QString> &) -> QString {
             this->showSearch(false);
             return "";
         }},
        {"reconnect",
         [this](const std::vector<QString> &) -> QString {
             this->reconnect();
             return "";
         }},
        {"debug",
         [](const std::vector<QString> &) -> QString {
             auto *popup = new DebugPopup;
             popup->setAttribute(Qt::WA_DeleteOnClose);
             popup->setWindowTitle("Leafyrino - Debug popup");
             popup->show();
             return "";
         }},
        {"focus",
         [this](const std::vector<QString> &arguments) -> QString {
             if (arguments.empty())
             {
                 return "focus action requires only one argument: the "
                        "focus direction Use \"up\", \"above\", \"down\", "
                        "\"below\", \"left\" or \"right\".";
             }
             const auto &direction = arguments.at(0);
             if (direction == "up" || direction == "above")
             {
                 this->actionRequested.invoke(Action::SelectSplitAbove);
             }
             else if (direction == "down" || direction == "below")
             {
                 this->actionRequested.invoke(Action::SelectSplitBelow);
             }
             else if (direction == "left")
             {
                 this->actionRequested.invoke(Action::SelectSplitLeft);
             }
             else if (direction == "right")
             {
                 this->actionRequested.invoke(Action::SelectSplitRight);
             }
             else
             {
                 return "focus in unknown direction. Use \"up\", "
                        "\"above\", \"down\", \"below\", \"left\" or "
                        "\"right\".";
             }
             return "";
         }},
        {"scrollToBottom",
         [this](const std::vector<QString> &) -> QString {
             this->getChannelView().getScrollBar().scrollToBottom(
                 getSettings()->enableSmoothScrollingNewMessages.getValue());
             return "";
         }},
        {"scrollToTop",
         [this](const std::vector<QString> &) -> QString {
             this->getChannelView().getScrollBar().scrollToTop(
                 getSettings()->enableSmoothScrollingNewMessages.getValue());
             return "";
         }},
        {"scrollPage",
         [this](const std::vector<QString> &arguments) -> QString {
             if (arguments.empty())
             {
                 qCWarning(chatterinoHotkeys)
                     << "scrollPage hotkey called without arguments!";
                 return "scrollPage hotkey called without arguments!";
             }
             const auto &direction = arguments.at(0);

             auto &scrollbar = this->getChannelView().getScrollBar();
             if (direction == "up")
             {
                 scrollbar.offset(-scrollbar.getPageSize());
             }
             else if (direction == "down")
             {
                 scrollbar.offset(scrollbar.getPageSize());
             }
             else
             {
                 qCWarning(chatterinoHotkeys) << "Unknown scroll direction";
             }
             return "";
         }},
        {"pickFilters",
         [this](const std::vector<QString> &) -> QString {
             this->setFiltersDialog();
             return "";
         }},
        {"openInBrowser",
         [this](const std::vector<QString> &) -> QString {
             if (this->getChannel()->getType() == Channel::Type::TwitchWhispers)
             {
                 this->openWhispersInBrowser();
             }
             else
             {
                 this->openInBrowser();
             }

             return "";
         }},
        {"openInStreamlink",
         [this](const std::vector<QString> &) -> QString {
             this->openInStreamlink();
             return "";
         }},
        {"openInCustomPlayer",
         [this](const std::vector<QString> &) -> QString {
             this->openWithCustomScheme();
             return "";
         }},
        {"openPlayerInBrowser",
         [this](const std::vector<QString> &) -> QString {
             this->openBrowserPlayer();
             return "";
         }},
        {"openModView",
         [this](const std::vector<QString> &) -> QString {
             this->openModViewInBrowser();
             return "";
         }},
        {"createClip",
         [this](const std::vector<QString> &) -> QString {
             // Alt+X: create clip LUL
             auto channel = this->getSelectedChannel();
             if (const auto type = channel->getType();
                 type != Channel::Type::Twitch &&
                 type != Channel::Type::TwitchWatching)
             {
                 return "Cannot create clips in a non-Twitch channel.";
             }

             auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get());

             twitchChannel->createClip({}, {});
             return "";
         }},
        {"reloadEmotes",
         [this](const std::vector<QString> &arguments) -> QString {
             auto reloadChannel = true;
             auto reloadSubscriber = true;
             auto reloadBadges = true;
             if (!arguments.empty())
             {
                 const auto &arg = arguments.at(0);
                 if (arg == "channel")
                 {
                     reloadSubscriber = false;
                     reloadBadges = false;
                 }
                 else if (arg == "subscriber")
                 {
                     reloadChannel = false;
                     reloadBadges = false;
                 }
             }

             if (reloadChannel)
             {
                 this->header_->reloadChannelEmotes();
             }
             if (reloadSubscriber)
             {
                 this->header_->reloadSubscriberEmotes();
             }

             if (reloadBadges)
             {
                 getApp()->getTwitchBadges()->loadTwitchBadges(
                     this->getChannel());
             }

             return "";
         }},
        {"setModerationMode",
         [this](const std::vector<QString> &arguments) -> QString {
             if (!this->getSelectedChannel()->isTwitchOrKickChannel())
             {
                 return "Cannot set moderation mode in a non-Twitch "
                        "channel.";
             }
             auto mode = 2;
             // 0 is off
             // 1 is on
             // 2 is toggle
             if (!arguments.empty())
             {
                 const auto &arg = arguments.at(0);
                 if (arg == "off")
                 {
                     mode = 0;
                 }
                 else if (arg == "on")
                 {
                     mode = 1;
                 }
             }

             switch (mode)
             {
                 case 0:
                     this->setModerationMode(false);
                     break;
                 case 1:
                     this->setModerationMode(true);
                     break;
                 default:
                     this->setModerationMode(!this->getModerationMode());
             }

             return "";
         }},
        {"openViewerList",
         [this](const std::vector<QString> &) -> QString {
             this->openChatterList();
             return "";
         }},
        {"clearMessages",
         [this](const std::vector<QString> &) -> QString {
             this->clear();
             return "";
         }},
        {"runCommand",
         [this](const std::vector<QString> &arguments) -> QString {
             if (arguments.empty())
             {
                 qCWarning(chatterinoHotkeys)
                     << "runCommand hotkey called without arguments!";
                 return "runCommand hotkey called without arguments!";
             }
             QString requestedText = QString(arguments[0]).replace('\n', ' ');

             QString inputText = this->getInput().getInputText();
             QString message = getApp()->getCommands()->execCustomCommand(
                 requestedText.split(' '), Command{"(hotkey)", requestedText},
                 true, this->getChannel(), nullptr,
                 {
                     {"input.text", inputText},
                 });

             message = getApp()->getCommands()->execCommand(
                 message, this->getChannel(), false);
             this->getChannel()->sendMessage(message);
             return "";
         }},
        {"setChannelNotification",
         [this](const std::vector<QString> &arguments) -> QString {
             auto channel = this->getSelectedChannel();
             if (!channel->isTwitchChannel())
             {
                 return "Cannot set channel notifications for a non-Twitch "
                        "channel.";
             }
             auto mode = 2;
             // 0 is off
             // 1 is on
             // 2 is toggle
             if (!arguments.empty())
             {
                 const auto &arg = arguments.at(0);
                 if (arg == "off")
                 {
                     mode = 0;
                 }
                 else if (arg == "on")
                 {
                     mode = 1;
                 }
             }

             auto *notifications = getApp()->getNotifications();
             const QString channelName = channel->getName();
             switch (mode)
             {
                 case 0:
                     notifications->removeChannelNotification(channelName,
                                                              Platform::Twitch);
                     break;
                 case 1:
                     notifications->addChannelNotification(channelName,
                                                           Platform::Twitch);
                     break;
                 default:
                     notifications->updateChannelNotification(channelName,
                                                              Platform::Twitch);
             }
             return "";
         }},
        {"popupOverlay",
         [this](const auto &) -> QString {
             this->showOverlayWindow();
             return {};
         }},
        {"toggleOverlayInertia",
         [this](const auto &args) -> QString {
             if (args.empty())
             {
                 return "No arguments provided to toggleOverlayInertia "
                        "(expected one)";
             }
             const auto &arg = args.front();

             if (arg == "this")
             {
                 if (this->overlayWindow_)
                 {
                     this->overlayWindow_->toggleInertia();
                 }
                 return {};
             }
             if (arg == "thisOrAll")
             {
                 if (this->overlayWindow_)
                 {
                     this->overlayWindow_->toggleInertia();
                 }
                 else
                 {
                     getApp()->getWindows()->toggleAllOverlayInertia();
                 }
                 return {};
             }
             if (arg == "all")
             {
                 getApp()->getWindows()->toggleAllOverlayInertia();
                 return {};
             }
             return {};
         }},
        {"setHighlightSounds",
         [this](const std::vector<QString> &arguments) -> QString {
             auto channelPtr = this->getSelectedChannel();
             if (!channelPtr->isTwitchChannel())
             {
                 return "Cannot set highlight sounds in a non-Twitch "
                        "channel.";
             }

             auto mode = 2;
             // 0 is off
             // 1 is on
             // 2 is toggle
             if (!arguments.empty())
             {
                 const auto &arg = arguments.at(0);
                 if (arg == "off")
                 {
                     mode = 0;
                 }
                 else if (arg == "on")
                 {
                     mode = 1;
                 }
             }

             const QString channel = channelPtr->getName();

             switch (mode)
             {
                 case 0:
                     getSettings()->mute(channel);
                     break;
                 case 1:
                     getSettings()->unmute(channel);
                     break;
                 default:
                     getSettings()->toggleMutedChannel(channel);
             }
             return "";
         }},
        {"openSubscriptionPage",
         [this](const auto &) -> QString {
             if (!this->getSelectedChannel()->isTwitchChannel())
             {
                 return "Cannot subscribe to a non-Twitch "
                        "channel.";
             }

             this->openSubPage();
             return "";
         }},
        {"changeMultichannelContext",
         [this](const std::vector<QString> &arguments) -> QString {
             if (arguments.empty())
             {
                 return "Expected at least one argument";
             }
             auto *mc =
                 dynamic_cast<MultiChannel *>(this->channel_.get().get());
             if (!mc || mc->channels().empty())
             {
                 return {};
             }

             size_t nextIndex = mc->activeChannelIndex();
             QStringView arg = arguments[0];
             if (arg == u"next")
             {
                 nextIndex =
                     (mc->activeChannelIndex() + 1) % mc->channels().size();
             }
             else if (arg == u"prev")
             {
                 if (mc->activeChannelIndex() == 0)
                 {
                     nextIndex = mc->channels().size() - 1;
                 }
                 else
                 {
                     nextIndex -= 1;
                 }
             }
             else
             {
                 bool ok = false;
                 nextIndex = arg.toULongLong(&ok);
                 if (!ok)
                 {
                     return "Failed to parse argument as integer";
                 }
             }
             mc->setActiveChannelIndex(nextIndex);
             getApp()->getWindows()->forceLayoutChannelViews();
             return {};
         }},
    };

    this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
        HotkeyCategory::Split, actions, this);
}

void Split::showEvent(QShowEvent *event)
{
    BaseWidget::showEvent(event);
    this->scheduleDeferredTwitchRefresh(!shouldUseColdTwitchFeatureDelay());
}

Split::~Split()
{
    this->usermodeChangedConnection_.disconnect();
    this->roomModeChangedConnection_.disconnect();
    this->channelIDChangedConnection_.disconnect();
    this->indirectChannelChangedConnection_.disconnect();
}

void Split::scheduleDeferredTwitchRefresh(bool interactive)
{
    if (this->deferredTwitchRefreshTimer_ == nullptr)
    {
        return;
    }

    if (dynamic_cast<TwitchChannel *>(this->getSelectedChannel().get()) ==
        nullptr)
    {
        return;
    }

    auto *container = dynamic_cast<SplitContainer *>(this->parentWidget());
    if (container != nullptr && container->getSelectedSplit() != nullptr &&
        container->getSelectedSplit() != this)
    {
        return;
    }

    const int delayMs = interactive
                            ? INTERACTIVE_TWITCH_FEATURE_REFRESH_DELAY_MS
                            : DEFERRED_TWITCH_FEATURE_REFRESH_DELAY_MS;

    if (this->deferredTwitchRefreshTimer_->isActive())
    {
        const int remaining =
            this->deferredTwitchRefreshTimer_->remainingTime();
        if (remaining >= 0 && remaining <= delayMs)
        {
            this->deferredTwitchRefreshInteractive_ =
                this->deferredTwitchRefreshInteractive_ || interactive;
            return;
        }
    }

    this->deferredTwitchRefreshRetries_ = 0;
    this->deferredTwitchRefreshInteractive_ = interactive;
    this->deferredTwitchRefreshTimer_->start(delayMs);
}

void Split::runDeferredTwitchRefresh()
{
    const bool interactive = this->deferredTwitchRefreshInteractive_;

    if (!this->isVisible())
    {
        return;
    }

    auto *container = dynamic_cast<SplitContainer *>(this->parentWidget());
    if (container != nullptr && container->getSelectedSplit() != nullptr &&
        container->getSelectedSplit() != this)
    {
        return;
    }

    auto *channel =
        dynamic_cast<TwitchChannel *>(this->getSelectedChannel().get());
    if (channel == nullptr)
    {
        return;
    }

    if (channel->roomId().isEmpty())
    {
        if (this->deferredTwitchRefreshRetries_ <
            DEFERRED_TWITCH_ROOM_ID_MAX_RETRIES)
        {
            ++this->deferredTwitchRefreshRetries_;
            this->deferredTwitchRefreshTimer_->start(
                interactive ? INTERACTIVE_TWITCH_ROOM_ID_RETRY_MS
                            : DEFERRED_TWITCH_ROOM_ID_RETRY_MS);
        }
        return;
    }

    this->deferredTwitchRefreshRetries_ = 0;
    this->deferredTwitchRefreshInteractive_ = false;
    const bool forcePersonalRefresh = this->deferredTwitchForcePersonalRefresh_;
    this->deferredTwitchForcePersonalRefresh_ = false;

    const int pollOffsetMs = interactive
                                 ? INTERACTIVE_TWITCH_POLL_REFRESH_OFFSET_MS
                                 : DEFERRED_TWITCH_POLL_REFRESH_OFFSET_MS;
    const int pointsOffsetMs = interactive
                                   ? INTERACTIVE_TWITCH_POINTS_REFRESH_OFFSET_MS
                                   : DEFERRED_TWITCH_POINTS_REFRESH_OFFSET_MS;
    const int warningOffsetMs =
        interactive ? INTERACTIVE_TWITCH_WARNING_REFRESH_OFFSET_MS
                    : DEFERRED_TWITCH_WARNING_REFRESH_OFFSET_MS;
    const int chattersOffsetMs =
        interactive ? INTERACTIVE_TWITCH_CHATTERS_REFRESH_OFFSET_MS
                    : DEFERRED_TWITCH_CHATTERS_REFRESH_OFFSET_MS;

    auto runIfStillActive = [this](auto &&callback) {
        if (!this->isVisible())
        {
            return;
        }

        auto *container = dynamic_cast<SplitContainer *>(this->parentWidget());
        if (container != nullptr && container->getSelectedSplit() != nullptr &&
            container->getSelectedSplit() != this)
        {
            return;
        }

        auto *tc =
            dynamic_cast<TwitchChannel *>(this->getSelectedChannel().get());
        if (tc == nullptr || tc->roomId().isEmpty())
        {
            return;
        }

        callback(tc);
    };

    channel->showPendingChatWarningIfVisible();
    const bool shouldRefreshWarnings =
        interactive || this->deferredTwitchWarningStartupSeen_;
    this->deferredTwitchWarningStartupSeen_ = true;

    if (getSettings()->enablePredictions)
    {
        runIfStillActive([forcePersonalRefresh](TwitchChannel *tc) {
            tc->refreshPrediction(forcePersonalRefresh);
        });
    }
    if (getSettings()->enablePolls)
    {
        QTimer::singleShot(
            pollOffsetMs, this, [this, runIfStillActive, forcePersonalRefresh] {
                runIfStillActive([forcePersonalRefresh](TwitchChannel *tc) {
                    tc->refreshPollIfStale(forcePersonalRefresh);
                });
            });
    }
    if (getSettings()->enableChannelPointsDisplay)
    {
        QTimer::singleShot(
            pointsOffsetMs, this,
            [this, runIfStillActive, forcePersonalRefresh] {
                runIfStillActive([forcePersonalRefresh](TwitchChannel *tc) {
                    tc->refreshChannelPointsIfStale(forcePersonalRefresh);
                });
            });
    }
    if (shouldRefreshWarnings)
    {
        QTimer::singleShot(
            warningOffsetMs, this,
            [this, runIfStillActive, forcePersonalRefresh] {
                runIfStillActive([forcePersonalRefresh](TwitchChannel *tc) {
                    tc->refreshChatWarningIfStale(forcePersonalRefresh);
                });
            });
    }
    QTimer::singleShot(chattersOffsetMs, this, [this, runIfStillActive] {
        runIfStillActive([](TwitchChannel *tc) {
            tc->refreshChatters();
        });
    });
}

ChannelView &Split::getChannelView()
{
    return *this->view_;
}

SplitInput &Split::getInput()
{
    return *this->input_;
}

MessageView *Split::pinnedExpandedMessageView() const
{
    return nullptr;
}

void Split::updateInputPlaceholder()
{
    // If the user disabled placeholder text, clear it and bail out
    if (!getSettings()->showInputPlaceholder)
    {
        this->input_->ui_.textEdit->setPlaceholderText({});
        return;
    }

    auto channel = this->getChannel();
    if (auto *multiChannel = dynamic_cast<MultiChannel *>(channel.get()))
    {
        const auto *active = multiChannel->activeChannel();
        if (!active)
        {
            this->input_->ui_.textEdit->setPlaceholderText({});
        }
        else
        {
            this->input_->ui_.textEdit->setPlaceholderText(
                u"Send message in " % active->channel->getName() % u"...");
        }
        return;
    }
    if (this->getChannel()->isKickChannel())
    {
        auto user = getApp()->getAccounts()->kick.current();
        QString placeholderText = [&] {
            if (user->isAnonymous())
            {
                // FIXME: once we have a proper OAuth for Kick (device auth or similar),
                // we can update this label to "Log in to send messages...".
                return QString{};
            }
            return QString(u"Send message as " % user->username() % u"...");
        }();
        this->input_->ui_.textEdit->setPlaceholderText(placeholderText);
        return;
    }

    if (!this->getChannel()->isTwitchChannel())
    {
        return;
    }

    if (auto *twitchChannel =
            dynamic_cast<TwitchChannel *>(this->getChannel().get()))
    {
        if (twitchChannel->isAnonymous())
        {
            this->input_->ui_.textEdit->setPlaceholderText(
                "Anonymous channel - read only");
            return;
        }
    }

    auto user = getApp()->getAccounts()->twitch.getCurrent();
    QString placeholderText;

    if (user->isAnon())
    {
        placeholderText = "Log in to send messages...";
    }
    else
    {
        placeholderText =
            QString("Send message as %1 in %2...")
                .arg(
                    getApp()->getAccounts()->twitch.getCurrent()->getUserName(),
                    this->getChannel()->getName());
    }

    this->input_->ui_.textEdit->setPlaceholderText(placeholderText);
}

void Split::joinChannelInNewTab(const ChannelPtr &channel)
{
    auto &nb = getApp()->getWindows()->getMainWindow().getNotebook();
    SplitContainer *container = nb.addPage(true);

    auto *split = new Split(container);
    split->setChannel(channel);
    container->insertSplit(split);
}

void Split::refreshModerationMode()
{
    this->header_->updateIcons();
    this->view_->queueLayout();
}

namespace {

QString pinBannerKey(const std::optional<TwitchChannel::PinnedMessage> &pin)
{
    if (!pin)
    {
        return {};
    }

    QString key = !pin->pinId.isEmpty() ? pin->pinId : pin->messageId;
    if (key.isEmpty())
    {
        key = pin->authorLogin + QStringLiteral(":") + pin->text.left(80);
    }

    if (pin->endsAt && pin->endsAt->isValid())
    {
        key += QStringLiteral("|") + pin->endsAt->toUTC().toString(Qt::ISODate);
    }
    return key;
}

QString predictionBannerKey(
    const std::optional<TwitchChannel::PredictionEvent> &prediction)
{
    if (!prediction)
    {
        return {};
    }

    return prediction->id + QStringLiteral("|") + prediction->status.toUpper() +
           QStringLiteral("|") + prediction->winningOutcomeId;
}

QString pollBannerKey(const std::optional<TwitchChannel::PollEvent> &poll)
{
    if (!poll)
    {
        return {};
    }

    return poll->id + QStringLiteral("|") + poll->status.toUpper();
}

bool predictionIsActive(
    const std::optional<TwitchChannel::PredictionEvent> &prediction)
{
    return prediction &&
           prediction->status.compare("ACTIVE", Qt::CaseInsensitive) == 0;
}

bool pollIsActive(const std::optional<TwitchChannel::PollEvent> &poll)
{
    return poll && poll->status.compare("ACTIVE", Qt::CaseInsensitive) == 0;
}

}  // namespace

void Split::clearBannerAttention()
{
    this->bannerAttentionOverride_ = -1;
    this->bannerAttentionUntil_ = {};
}

void Split::noteBannerStateChanged(TwitchChannel *channel, int bannerId)
{
    if (channel == nullptr)
    {
        this->lastPinBannerKey_.clear();
        this->lastPredictionBannerKey_.clear();
        this->lastPollBannerKey_.clear();
        this->clearBannerAttention();
        return;
    }

    const auto pin = *channel->accessPinnedMessage();
    const auto prediction = *channel->accessPrediction();
    const auto poll = *channel->accessPoll();

    const auto newPinKey = pinBannerKey(pin);
    const auto newPredictionKey = predictionBannerKey(prediction);
    const auto newPollKey = pollBannerKey(poll);

    QString *oldKey = nullptr;
    QString newKey;
    switch (bannerId)
    {
        case 0:
            oldKey = &this->lastPinBannerKey_;
            newKey = newPinKey;
            break;
        case 1:
            oldKey = &this->lastPredictionBannerKey_;
            newKey = newPredictionKey;
            break;
        case 2:
            oldKey = &this->lastPollBannerKey_;
            newKey = newPollKey;
            break;
        default:
            return;
    }

    const bool changed = oldKey != nullptr && *oldKey != newKey;

    this->lastPinBannerKey_ = newPinKey;
    this->lastPredictionBannerKey_ = newPredictionKey;
    this->lastPollBannerKey_ = newPollKey;

    if (this->primingBannerState_ || !changed || newKey.isEmpty() ||
        getSettings()->bannerStackMode != 3)
    {
        return;
    }

    const bool hasLivePrediction = predictionIsActive(prediction);
    const bool hasLivePoll = pollIsActive(poll);
    int durationMs = 5000;

    if (bannerId == 0)
    {
        durationMs = (hasLivePrediction || hasLivePoll) ? 5000 : 20000;
    }
    else if (bannerId == 1 && prediction)
    {
        const auto status = prediction->status.toUpper();
        if (status == "ACTIVE")
        {
            durationMs = 12000;
        }
        else if (status == "LOCKED")
        {
            durationMs = 6000;
        }
        else if (status == "RESOLVED")
        {
            durationMs = 10000;
        }
        else
        {
            durationMs = 5000;
        }
    }
    else if (bannerId == 2 && poll)
    {
        const auto status = poll->status.toUpper();
        if (status == "ACTIVE")
        {
            durationMs = 12000;
        }
        else if (status == "COMPLETED")
        {
            durationMs = 8000;
        }
        else
        {
            durationMs = 5000;
        }
    }

    this->bannerToggleOverride_ = -1;
    this->bannerAttentionOverride_ = bannerId;
    this->bannerAttentionUntil_ =
        QDateTime::currentDateTimeUtc().addMSecs(durationMs);

    QTimer::singleShot(durationMs + 50, this, [this, bannerId] {
        if (this->bannerAttentionOverride_ == bannerId &&
            this->bannerAttentionUntil_.isValid() &&
            QDateTime::currentDateTimeUtc() >= this->bannerAttentionUntil_)
        {
            this->clearBannerAttention();
            this->updateBannerVisibility();
        }
    });
}

void Split::updateBannerVisibility()
{
    const bool hasPin = this->pinnedBanner_->hasPinnedMessage() &&
                        !this->perSplitHidePinnedMessage_;
    const bool hasPred = this->predictionBanner_->hasPrediction() &&
                         !this->perSplitHidePrediction_;
    const int mode = getSettings()->bannerStackMode;
    const bool hasPoll =
        this->pollBanner_->hasPoll() && !this->perSplitHidePoll_;

    const int activeCount = int(hasPin) + int(hasPred) + int(hasPoll);
    auto setVisibility = [this](bool showPin, bool showPred, bool showPoll,
                                bool showToggle) {
        this->pinnedBanner_->setVisible(showPin);
        this->predictionBanner_->setVisible(showPred);
        this->pollBanner_->setVisible(showPoll);

        this->pinnedBanner_->setToggleButtonVisible(showToggle && showPin);
        this->predictionBanner_->setToggleButtonVisible(showToggle && showPred);
        this->pollBanner_->setToggleButtonVisible(showToggle && showPoll);
    };

    if (mode == 0 || activeCount <= 1)
    {
        setVisibility(hasPin, hasPred, hasPoll, false);
        return;
    }

    QVector<int> activeBannerIds;
    if (hasPin)
    {
        activeBannerIds.push_back(0);
    }
    if (hasPred)
    {
        activeBannerIds.push_back(1);
    }
    if (hasPoll)
    {
        activeBannerIds.push_back(2);
    }

    auto firstActiveFromOrder =
        [&activeBannerIds](std::initializer_list<int> order) {
            for (const int id : order)
            {
                if (activeBannerIds.contains(id))
                {
                    return id;
                }
            }
            return activeBannerIds.isEmpty() ? -1 : activeBannerIds.front();
        };

    int selectedId = -1;
    if (this->bannerAttentionOverride_ >= 0)
    {
        if (this->bannerAttentionUntil_.isValid() &&
            QDateTime::currentDateTimeUtc() < this->bannerAttentionUntil_ &&
            activeBannerIds.contains(this->bannerAttentionOverride_))
        {
            selectedId = this->bannerAttentionOverride_;
        }
        else
        {
            this->clearBannerAttention();
        }
    }

    if (selectedId < 0 && this->bannerToggleOverride_ >= 0 &&
        activeBannerIds.contains(this->bannerToggleOverride_))
    {
        selectedId = this->bannerToggleOverride_;
    }
    else if (selectedId < 0 && mode == 1)
    {
        selectedId = firstActiveFromOrder({0, 1, 2});
    }
    else if (selectedId < 0 && mode == 2)
    {
        selectedId = firstActiveFromOrder({1, 2, 0});
    }
    else if (selectedId < 0 && mode == 4)
    {
        selectedId = firstActiveFromOrder({2, 1, 0});
    }
    else if (selectedId < 0)
    {
        auto urgencyBonus = [](qint64 secondsLeft) {
            if (secondsLeft <= 0)
            {
                return 30;
            }
            if (secondsLeft <= 15)
            {
                return 30;
            }
            if (secondsLeft <= 60)
            {
                return 20;
            }
            if (secondsLeft <= 120)
            {
                return 12;
            }
            if (secondsLeft <= 300)
            {
                return 5;
            }
            return 0;
        };

        auto pollSelfVoteCount = [](const TwitchChannel::PollEvent &poll) {
            int total = 0;
            for (const auto &vote : poll.selfVotes)
            {
                total += vote.freeVotes + vote.channelPointsVotes;
            }
            return total;
        };

        const auto now = QDateTime::currentDateTimeUtc();
        int pinScore = hasPin ? 58 : -1;
        int predictionScore = hasPred ? 0 : -1;
        int pollScore = hasPoll ? 0 : -1;

        if (auto *tc =
                dynamic_cast<TwitchChannel *>(this->channel_.get().get()))
        {
            if (hasPin)
            {
                auto pinGuard = tc->accessPinnedMessage();
                if (*pinGuard)
                {
                    const auto &pin = **pinGuard;
                    if (pin.pinnedAt && pin.pinnedAt->isValid() &&
                        pin.pinnedAt->secsTo(now) <= 120)
                    {
                        pinScore += 12;
                    }
                    if (pin.endsAt && pin.endsAt->isValid())
                    {
                        pinScore += urgencyBonus(now.secsTo(*pin.endsAt)) / 2;
                    }
                }
            }

            if (hasPred)
            {
                auto predictionGuard = tc->accessPrediction();
                if (*predictionGuard)
                {
                    const auto &prediction = **predictionGuard;
                    const auto status = prediction.status.toUpper();
                    if (status == "RESOLVED")
                    {
                        predictionScore = 52;
                    }
                    else if (status == "LOCKED")
                    {
                        predictionScore = 36;
                    }
                    else if (status == "CANCELED")
                    {
                        predictionScore = 25;
                    }
                    else if (status == "ACTIVE")
                    {
                        predictionScore = 90;
                        if (prediction.selfOutcomeId.isEmpty())
                        {
                            predictionScore += 15;
                        }
                        if (prediction.createdAt.isValid() &&
                            prediction.predictionWindowSeconds > 0)
                        {
                            const auto end = prediction.createdAt.addSecs(
                                prediction.predictionWindowSeconds);
                            predictionScore += urgencyBonus(now.secsTo(end));
                        }
                    }
                    else
                    {
                        predictionScore = 38;
                    }
                }
            }

            if (hasPoll)
            {
                auto pollGuard = tc->accessPoll();
                if (*pollGuard)
                {
                    const auto &poll = **pollGuard;
                    const auto status = poll.status.toUpper();
                    if (status == "ACTIVE")
                    {
                        pollScore = 88;
                        if (pollSelfVoteCount(poll) == 0)
                        {
                            pollScore += 14;
                        }
                        if (poll.channelPointsVotingEnabled)
                        {
                            pollScore += 4;
                        }

                        std::optional<QDateTime> end = poll.endsAt;
                        if (!end && poll.createdAt.isValid() &&
                            poll.durationSeconds > 0)
                        {
                            end = poll.createdAt.addSecs(poll.durationSeconds);
                        }
                        if (end && end->isValid())
                        {
                            pollScore += urgencyBonus(now.secsTo(*end));
                        }
                    }
                    else if (status == "COMPLETED")
                    {
                        pollScore = 50;
                    }
                    else if (status == "TERMINATED" || status == "ARCHIVED")
                    {
                        pollScore = 25;
                    }
                    else
                    {
                        pollScore = 38;
                    }
                }
            }
        }

        struct Candidate {
            int id = -1;
            int score = -1;
            int tieBreak = 0;
        };

        Candidate best;
        const Candidate candidates[] = {
            {0, pinScore, 0},
            {1, predictionScore, 2},
            {2, pollScore, 1},
        };
        for (const auto &candidate : candidates)
        {
            if (!activeBannerIds.contains(candidate.id))
            {
                continue;
            }
            if (candidate.score > best.score ||
                (candidate.score == best.score &&
                 candidate.tieBreak > best.tieBreak))
            {
                best = candidate;
            }
        }

        selectedId = best.id >= 0 ? best.id : firstActiveFromOrder({1, 2, 0});
    }

    setVisibility(selectedId == 0, selectedId == 1, selectedId == 2, true);
}

void Split::openChannelInBrowserPlayer(ChannelPtr channel)
{
    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        QDesktopServices::openUrl(
            QUrl(TWITCH_PLAYER_URL.arg(twitchChannel->getName())));
    }
}

void Split::openChannelInStreamlink(const QString channelName)
{
    try
    {
        openStreamlinkForChannel(channelName);
    }
    catch (const Exception &ex)
    {
        qCWarning(chatterinoWidget)
            << "Error in doOpenStreamlink:" << ex.what();
    }
}

void Split::openChannelInCustomPlayer(const QString channelName)
{
    openInCustomPlayer(channelName);
}

IndirectChannel Split::getIndirectChannel()
{
    return this->channel_;
}

ChannelPtr Split::getChannel() const
{
    return this->channel_.get();
}

ChannelPtr Split::getSelectedChannel() const
{
    ChannelPtr chan = this->channel_.get();
    auto *multiChannel = dynamic_cast<MultiChannel *>(chan.get());
    if (multiChannel)
    {
        const auto *active = multiChannel->activeChannel();
        if (active)
        {
            chan = active->channel;
        }
    }
    return chan;
}

void Split::setChannel(IndirectChannel newChannel)
{
    this->channel_ = newChannel;

    this->view_->setChannel(newChannel.get());
    this->channelSignalHolder_.clear();
    this->bannerToggleOverride_ = -1;
    this->clearBannerAttention();
    this->lastPinBannerKey_.clear();
    this->lastPredictionBannerKey_.clear();
    this->lastPollBannerKey_.clear();
    this->primingBannerState_ = true;
    this->pinnedBanner_->setPinnedMessage(std::nullopt, nullptr);
    this->predictionBanner_->setPrediction(std::nullopt, nullptr);
    this->pollBanner_->setPoll(std::nullopt, nullptr);

    this->usermodeChangedConnection_.disconnect();
    this->roomModeChangedConnection_.disconnect();
    this->indirectChannelChangedConnection_.disconnect();
    this->channelSignalHolder_.clear();

    TwitchChannel *tc = dynamic_cast<TwitchChannel *>(newChannel.get().get());
    auto *kc = dynamic_cast<KickChannel *>(newChannel.get().get());
    auto *mc = dynamic_cast<MultiChannel *>(newChannel.get().get());

    if (mc)
    {
        this->channelSignalHolder_.managedConnect(
            mc->activeChannelChanged, [this] {
                this->updateInputPlaceholder();
                this->scheduleDeferredTwitchRefresh(true);
            });

        if (this->isVisible())
        {
            this->scheduleDeferredTwitchRefresh(
                !shouldUseColdTwitchFeatureDelay());
        }
    }
    else if (tc != nullptr)
    {
        this->usermodeChangedConnection_ = tc->userStateChanged.connect([this] {
            this->header_->updateIcons();
            this->header_->updateRoomModes();
        });

        this->roomModeChangedConnection_ = tc->roomModesChanged.connect([this] {
            this->header_->updateRoomModes();
        });

        auto updatePin = [this, tc] {
            this->noteBannerStateChanged(tc, 0);
            this->pinnedBanner_->setPinnedMessage(*tc->accessPinnedMessage(),
                                                  tc);
            this->updateBannerVisibility();
        };

        if (getSettings()->enablePinnedMessages)
        {
            this->channelSignalHolder_.managedConnect(tc->pinnedMessageChanged,
                                                      updatePin);

            this->channelSignalHolder_.managedConnect(
                tc->messageReplaced,
                [this, tc](size_t /*index*/, const MessagePtr & /*prev*/,
                           const MessagePtr &replacement) {
                    auto pin = tc->accessPinnedMessage();
                    if (!pin->has_value())
                    {
                        return;
                    }
                    if (!(*pin)->messageId.isEmpty() &&
                        replacement->id == (*pin)->messageId)
                    {
                        this->pinnedBanner_->setPinnedMessage(*pin, tc);
                    }
                });

            this->channelSignalHolder_.managedConnect(
                tc->messageAppended,
                [this, tc](MessagePtr &msg,
                           std::optional<MessageFlags> /*flags*/) {
                    auto pin = tc->accessPinnedMessage();
                    if (pin->has_value() && !(*pin)->authorLogin.isEmpty() &&
                        msg->loginName.compare((*pin)->authorLogin,
                                               Qt::CaseInsensitive) == 0)
                    {
                        this->pinnedBanner_->refreshLayout();
                    }
                });

            updatePin();
        }
        else
        {
            this->pinnedBanner_->hide();
        }

        getSettings()->enablePinnedMessages.connect(
            [this, tc](const bool &enabled, auto) {
                if (enabled)
                {
                    tc->refreshPinnedMessage();
                    this->channelSignalHolder_.managedConnect(
                        tc->pinnedMessageChanged, [this, tc] {
                            this->noteBannerStateChanged(tc, 0);
                            this->pinnedBanner_->setPinnedMessage(
                                *tc->accessPinnedMessage(), tc);
                            this->updateBannerVisibility();
                        });
                }
                else
                {
                    this->pinnedBanner_->setPinnedMessage(std::nullopt, tc);
                    this->updateBannerVisibility();
                }
            },
            this->channelSignalHolder_);

        if (getSettings()->enablePredictions)
        {
            auto updatePrediction = [this, tc] {
                this->noteBannerStateChanged(tc, 1);
                this->predictionBanner_->setPrediction(*tc->accessPrediction(),
                                                       tc);
                this->updateBannerVisibility();
            };
            this->channelSignalHolder_.managedConnect(tc->predictionChanged,
                                                      updatePrediction);
            updatePrediction();
        }
        else
        {
            this->predictionBanner_->setPrediction(std::nullopt, tc);
        }

        if (getSettings()->enablePolls)
        {
            auto updatePoll = [this, tc] {
                this->noteBannerStateChanged(tc, 2);
                this->pollBanner_->setPoll(*tc->accessPoll(), tc);
                this->updateBannerVisibility();
            };
            this->channelSignalHolder_.managedConnect(tc->pollChanged,
                                                      updatePoll);
            updatePoll();
        }
        else
        {
            this->pollBanner_->setPoll(std::nullopt, tc);
        }

        auto weakTC = std::weak_ptr<TwitchChannel>(
            std::static_pointer_cast<TwitchChannel>(newChannel.get()));

        this->channelSignalHolder_.managedConnect(
            this->focused, [weakTC, this] {
                auto tc = weakTC.lock();
                if (!tc)
                {
                    return;
                }

                this->scheduleDeferredTwitchRefresh(true);
            });

        if (this->isVisible())
        {
            this->scheduleDeferredTwitchRefresh(
                !shouldUseColdTwitchFeatureDelay());
        }

        getSettings()->enablePredictions.connect(
            [this, tc](const bool &enabled, auto) {
                if (enabled)
                {
                    this->scheduleDeferredTwitchRefresh(true);
                    this->channelSignalHolder_.managedConnect(
                        tc->predictionChanged, [this, tc] {
                            this->noteBannerStateChanged(tc, 1);
                            this->predictionBanner_->setPrediction(
                                *tc->accessPrediction(), tc);
                            this->updateBannerVisibility();
                        });
                }
                else
                {
                    this->predictionBanner_->setPrediction(std::nullopt, tc);
                    this->updateBannerVisibility();
                }
            },
            this->channelSignalHolder_);

        getSettings()->enablePolls.connect(
            [this, tc](const bool &enabled, auto) {
                if (enabled)
                {
                    this->scheduleDeferredTwitchRefresh(true);
                    this->channelSignalHolder_.managedConnect(
                        tc->pollChanged, [this, tc] {
                            this->noteBannerStateChanged(tc, 2);
                            this->pollBanner_->setPoll(*tc->accessPoll(), tc);
                            this->updateBannerVisibility();
                        });
                }
                else
                {
                    this->pollBanner_->setPoll(std::nullopt, tc);
                    this->updateBannerVisibility();
                }
            },
            this->channelSignalHolder_);

        this->channelSignalHolder_.managedConnect(
            this->pinnedBanner_->toggleBannerRequested, [this] {
                this->clearBannerAttention();
                QVector<int> activeIds;
                if (this->pinnedBanner_->hasPinnedMessage())
                {
                    activeIds.push_back(0);
                }
                if (this->predictionBanner_->hasPrediction())
                {
                    activeIds.push_back(1);
                }
                if (this->pollBanner_->hasPoll())
                {
                    activeIds.push_back(2);
                }
                if (!activeIds.isEmpty())
                {
                    const int foundIndex =
                        activeIds.indexOf(this->bannerToggleOverride_);
                    const int currentIndex = foundIndex >= 0 ? foundIndex : 0;
                    this->bannerToggleOverride_ =
                        activeIds.at((currentIndex + 1) % activeIds.size());
                }
                this->updateBannerVisibility();
            });
        this->channelSignalHolder_.managedConnect(
            this->predictionBanner_->toggleBannerRequested, [this] {
                this->clearBannerAttention();
                QVector<int> activeIds;
                if (this->pinnedBanner_->hasPinnedMessage())
                {
                    activeIds.push_back(0);
                }
                if (this->predictionBanner_->hasPrediction())
                {
                    activeIds.push_back(1);
                }
                if (this->pollBanner_->hasPoll())
                {
                    activeIds.push_back(2);
                }
                if (!activeIds.isEmpty())
                {
                    const int foundIndex =
                        activeIds.indexOf(this->bannerToggleOverride_);
                    const int currentIndex = foundIndex >= 0 ? foundIndex : 0;
                    this->bannerToggleOverride_ =
                        activeIds.at((currentIndex + 1) % activeIds.size());
                }
                this->updateBannerVisibility();
            });
        this->channelSignalHolder_.managedConnect(
            this->pollBanner_->toggleBannerRequested, [this] {
                this->clearBannerAttention();
                QVector<int> activeIds;
                if (this->pinnedBanner_->hasPinnedMessage())
                {
                    activeIds.push_back(0);
                }
                if (this->predictionBanner_->hasPrediction())
                {
                    activeIds.push_back(1);
                }
                if (this->pollBanner_->hasPoll())
                {
                    activeIds.push_back(2);
                }
                if (!activeIds.isEmpty())
                {
                    const int foundIndex =
                        activeIds.indexOf(this->bannerToggleOverride_);
                    const int currentIndex = foundIndex >= 0 ? foundIndex : 0;
                    this->bannerToggleOverride_ =
                        activeIds.at((currentIndex + 1) % activeIds.size());
                }
                this->updateBannerVisibility();
            });

        this->channelSignalHolder_.managedConnect(
            this->pinnedBanner_->dismissed, [this] {
                this->bannerToggleOverride_ = -1;
                this->updateBannerVisibility();
            });
        this->channelSignalHolder_.managedConnect(
            this->predictionBanner_->dismissed, [this] {
                this->bannerToggleOverride_ = -1;
                this->updateBannerVisibility();
            });
        this->channelSignalHolder_.managedConnect(
            this->pollBanner_->dismissed, [this] {
                this->bannerToggleOverride_ = -1;
                this->updateBannerVisibility();
            });

        getSettings()->bannerStackMode.connect(
            [this](const int &, auto) {
                this->bannerToggleOverride_ = -1;
                this->updateBannerVisibility();
            },
            this->channelSignalHolder_);

        this->channelSignalHolder_.managedConnect(
            tc->sendWaitUpdate, [this](const QString &text) {
                this->getInput().setSendWaitStatus(text);
            });
    }
    else if (kc != nullptr)
    {
        this->usermodeChangedConnection_ = kc->userStateChanged.connect([this] {
            this->header_->updateIcons();
            this->header_->updateRoomModes();
        });

        this->roomModeChangedConnection_ = kc->roomModesChanged.connect([this] {
            this->header_->updateRoomModes();
        });

        this->channelSignalHolder_.managedConnect(
            kc->sendWaitUpdate, [this](const QString &text) {
                this->getInput().setSendWaitStatus(text);
            });
    }

    this->primingBannerState_ = false;

    this->indirectChannelChangedConnection_ =
        newChannel.getChannelChanged().connect([this] {
            QTimer::singleShot(0, [this] {
                this->setChannel(this->channel_);
            });
        });

    this->header_->updateIcons();
    this->header_->updateChannelText();
    this->header_->updateRoomModes();

    this->channelSignalHolder_.managedConnect(
        this->channel_.get()->displayNameChanged, [this] {
            this->actionRequested.invoke(Action::RefreshTab);
        });

    this->channelChanged.invoke();
    this->actionRequested.invoke(Action::RefreshTab);

    // Queue up save because: Split channel changed
    getApp()->getWindows()->queueSave();
}

void Split::setModerationMode(bool value)
{
    this->moderationMode_ = value;
    this->refreshModerationMode();
}

bool Split::getModerationMode() const
{
    return this->moderationMode_;
}

std::optional<bool> Split::checkSpellingOverride() const
{
    return this->input_->checkSpellingOverride();
}

void Split::setCheckSpellingOverride(std::optional<bool> override)
{
    this->input_->setCheckSpellingOverride(override);
}

void Split::syncPerSplitPanelHidesToPanels()
{
    this->updateBannerVisibility();
}

bool Split::perSplitHidePinnedMessage() const
{
    return this->perSplitHidePinnedMessage_;
}

void Split::setPerSplitHidePinnedMessage(bool hide)
{
    if (this->perSplitHidePinnedMessage_ == hide)
    {
        return;
    }
    this->perSplitHidePinnedMessage_ = hide;
    this->updateBannerVisibility();
    getApp()->getWindows()->queueSave();
}

bool Split::perSplitHidePrediction() const
{
    return this->perSplitHidePrediction_;
}

void Split::setPerSplitHidePrediction(bool hide)
{
    if (this->perSplitHidePrediction_ == hide)
    {
        return;
    }
    this->perSplitHidePrediction_ = hide;
    this->updateBannerVisibility();
    getApp()->getWindows()->queueSave();
}

bool Split::perSplitHidePoll() const
{
    return this->perSplitHidePoll_;
}

void Split::setPerSplitHidePoll(bool hide)
{
    if (this->perSplitHidePoll_ == hide)
    {
        return;
    }
    this->perSplitHidePoll_ = hide;
    this->updateBannerVisibility();
    getApp()->getWindows()->queueSave();
}

void Split::setPerSplitHideAllPanels(bool hide)
{
    if (this->perSplitHidePinnedMessage_ == hide &&
        this->perSplitHidePrediction_ == hide &&
        this->perSplitHidePoll_ == hide)
    {
        return;
    }
    this->perSplitHidePinnedMessage_ = hide;
    this->perSplitHidePrediction_ = hide;
    this->perSplitHidePoll_ = hide;
    this->syncPerSplitPanelHidesToPanels();
    getApp()->getWindows()->queueSave();
}

void Split::loadPerSplitPanelHides(bool hidePinned, bool hidePrediction,
                                   bool hidePoll)
{
    if (this->perSplitHidePinnedMessage_ == hidePinned &&
        this->perSplitHidePrediction_ == hidePrediction &&
        this->perSplitHidePoll_ == hidePoll)
    {
        return;
    }
    this->perSplitHidePinnedMessage_ = hidePinned;
    this->perSplitHidePrediction_ = hidePrediction;
    this->perSplitHidePoll_ = hidePoll;
    this->syncPerSplitPanelHidesToPanels();
}

void Split::insertTextToInput(const QString &text)
{
    this->input_->insertText(text);
}

void Split::showChangeChannelPopup(const char *dialogTitle, bool empty,
                                   std::function<void(bool)> callback)
{
    if (!this->selectChannelDialog_.isNull())
    {
        this->selectChannelDialog_->raise();

        return;
    }

    auto *dialog = new SelectChannelDialog(this);
    if (!empty)
    {
        dialog->setSelectedChannel(this->getIndirectChannel());
    }
    else
    {
        dialog->setSelectedChannel({});
    }
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(dialogTitle);
    dialog->show();
    // We can safely ignore this signal connection since the dialog will be closed before
    // this Split is closed
    std::ignore = dialog->closed.connect([=, this] {
        if (dialog->hasSeletedChannel())
        {
            this->setChannel(dialog->getSelectedChannel());
        }

        callback(dialog->hasSeletedChannel());
    });
    this->selectChannelDialog_ = dialog;
}

void Split::updateGifEmotes()
{
    this->view_->queueUpdate();
}

void Split::recoverDismissedPanels()
{
    this->bannerToggleOverride_ = -1;
    this->clearBannerAttention();
    this->scheduleDeferredTwitchRefresh(true);
    this->updateBannerVisibility();
}

void Split::updateLastReadMessage()
{
    this->view_->updateLastReadMessage();
}

void Split::paintEvent(QPaintEvent *)
{
    // color the background of the chat
    QPainter painter(this);

    painter.fillRect(this->rect(), this->theme->splits.background);
}

void Split::mouseMoveEvent(QMouseEvent *event)
{
    (void)event;

    this->handleModifiers(QGuiApplication::queryKeyboardModifiers());
}

void Split::keyPressEvent(QKeyEvent *event)
{
    (void)event;

    this->view_->unsetCursor();
    this->handleModifiers(QGuiApplication::queryKeyboardModifiers());
}

void Split::keyReleaseEvent(QKeyEvent *event)
{
    (void)event;

    this->view_->unsetCursor();
    this->handleModifiers(QGuiApplication::queryKeyboardModifiers());
}

void Split::resizeEvent(QResizeEvent *event)
{
    // Queue up save because: Split resized
    getApp()->getWindows()->queueSave();

    BaseWidget::resizeEvent(event);

    this->overlay_->setGeometry(this->rect());
    if (this->mpsOverlay_)
    {
        this->mpsOverlay_->setGeometry(this->rect());
        this->updateMpsOverlayAnchor();
    }
}

bool Split::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type())
    {
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::Show:
        case QEvent::Hide:
        case QEvent::LayoutRequest: {
            if (watched == this || watched == this->view_ ||
                watched == this->pinnedBanner_ ||
                watched == this->predictionBanner_ ||
                watched == this->pollBanner_)
            {
                this->updateMpsOverlayAnchor();
            }
        }
        break;
        default:
            break;
    }

    return BaseWidget::eventFilter(watched, event);
}

void Split::updateMpsOverlayAnchor()
{
    if (!this->mpsOverlay_)
    {
        return;
    }
    this->mpsOverlay_->setViewRect(this->view_->geometry());
}

void Split::enterEvent(QEnterEvent * /*event*/)
{
    this->isMouseOver_ = true;

    this->handleModifiers(QGuiApplication::queryKeyboardModifiers());

    if (modifierStatus ==
        SHOW_SPLIT_OVERLAY_MODIFIERS /*|| modifierStatus == showAddSplitRegions*/)
    {
        this->overlay_->show();
    }

    this->actionRequested.invoke(Action::ResetMouseStatus);
}

void Split::leaveEvent(QEvent *event)
{
    (void)event;

    this->isMouseOver_ = false;

    this->overlay_->hide();

    this->handleModifiers(QGuiApplication::queryKeyboardModifiers());
}

void Split::handleModifiers(Qt::KeyboardModifiers modifiers)
{
    if (modifierStatus != modifiers)
    {
        modifierStatus = modifiers;
        modifierStatusChanged.invoke(modifiers);
    }
}

void Split::setIsTopRightSplit(bool value)
{
    this->isTopRightSplit_ = value;
    this->header_->setAddButtonVisible(value);
}

/// Slots
void Split::addSibling()
{
    this->actionRequested.invoke(Action::AppendNewSplit);
}

void Split::deleteFromContainer()
{
    this->actionRequested.invoke(Action::Delete);
}

void Split::changeChannel()
{
    this->showChangeChannelPopup(
        "Change channel", false, [this](bool didSelectChannel) {
            if (!didSelectChannel)
            {
                return;
            }

            // After changing channel (i.e. pressing OK in the channel switcher), close all open Chatter Lists
            // We could consider updating the chatter list with the new channel
            for (const auto &w : this->findChildren<ChatterListWidget *>())
            {
                w->close();
            }
        });
}

void Split::explainMoving()
{
    showTutorialVideo(this, ":/examples/moving.gif", "Moving",
                      "Hold <Ctrl+Alt> to move splits.\n\nExample:");
}

void Split::explainSplitting()
{
    showTutorialVideo(this, ":/examples/splitting.gif", "Splitting",
                      "Hold <Ctrl+Alt> to add new splits.\n\nExample:");
}

void Split::popup()
{
    auto *app = getApp();
    Window &window = app->getWindows()->createWindow(WindowType::Popup);

    auto *split = new Split(window.getNotebook().getOrAddSelectedPage());

    split->setChannel(this->getIndirectChannel());
    split->setModerationMode(this->getModerationMode());
    split->setFilters(this->getFilters());

    window.getNotebook().getOrAddSelectedPage()->insertSplit(split);
    window.show();
}

OverlayWindow *Split::overlayWindow()
{
    return this->overlayWindow_.data();
}

void Split::showOverlayWindow()
{
    if (!this->overlayWindow_)
    {
        this->overlayWindow_ =
            new OverlayWindow(this->getIndirectChannel(), this->getFilters());
    }
    this->overlayWindow_->show();
}

void Split::clear()
{
    this->view_->clearMessages();
}

void Split::openInBrowser()
{
    auto channel = this->getSelectedChannel();

    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        QDesktopServices::openUrl("https://www.twitch.tv/" +
                                  twitchChannel->getName());
    }
    else if (auto *kc = dynamic_cast<KickChannel *>(channel.get()))
    {
        QDesktopServices::openUrl("https://kick.com/" + kc->slug());
    }
}

void Split::openWhispersInBrowser()
{
    auto userName = getApp()->getAccounts()->twitch.getCurrent()->getUserName();
    QDesktopServices::openUrl("https://www.twitch.tv/popout/moderator/" +
                              userName + "/whispers");
}

void Split::openBrowserPlayer()
{
    this->openChannelInBrowserPlayer(this->getSelectedChannel());
}

void Split::openModViewInBrowser()
{
    auto channel = this->getSelectedChannel();

    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        QDesktopServices::openUrl("https://www.twitch.tv/moderator/" +
                                  twitchChannel->getName());
    }
    else if (auto *kc = dynamic_cast<KickChannel *>(channel.get()))
    {
        QDesktopServices::openUrl("https://dashboard.kick.com/moderator/" +
                                  kc->slug());
    }
}

void Split::openInStreamlink()
{
    auto chan = this->getSelectedChannel();
    auto *kc = dynamic_cast<KickChannel *>(chan.get());
    if (kc)
    {
        openStreamlinkForChannel(kc->slug(), u"kick.com/");
        return;
    }
    this->openChannelInStreamlink(chan->getName());
}

void Split::openWithCustomScheme()
{
    auto *const channel = this->getSelectedChannel().get();
    if (auto *const twitchChannel = dynamic_cast<TwitchChannel *>(channel))
    {
        this->openChannelInCustomPlayer(twitchChannel->getName());
    }
    else if (auto *kc = dynamic_cast<KickChannel *>(channel))
    {
        openInCustomPlayer(kc->slug(), u"https://kick.com/");
    }
}

void Split::openChatterList()
{
    auto channel = this->getSelectedChannel();
    if (!channel)
    {
        qCWarning(chatterinoWidget)
            << "Chatter list opened when no channel was defined";
        return;
    }

    auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get());
    if (twitchChannel == nullptr)
    {
        qCWarning(chatterinoWidget)
            << "Chatter list opened in a non-Twitch channel";
        return;
    }

    const auto chatterListWidth = static_cast<int>(this->width() * 0.5);
    const auto chatterListHeight =
        this->height() - this->header_->height() - this->input_->height();

    auto *chatterDock = new ChatterListWidget(twitchChannel, this);

    QObject::connect(chatterDock, &ChatterListWidget::userClicked,
                     [this](const QString &userLogin) {
                         this->view_->showUserInfoPopup(userLogin);
                     });

    chatterDock->resize(chatterListWidth, chatterListHeight);
    widgets::showAndMoveWindowTo(
        chatterDock, this->mapToGlobal(QPoint{0, this->header_->height()}),
        widgets::BoundsChecking::CursorPosition);
}

void Split::openSubPage()
{
    ChannelPtr channel = this->getSelectedChannel();

    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        QDesktopServices::openUrl(twitchChannel->subscriptionUrl());
    }
}

void Split::setFiltersDialog()
{
    SelectChannelFiltersDialog d(this->getFilters(), this);
    d.setWindowTitle("Select filters");

    if (d.exec() == QDialog::Accepted)
    {
        this->setFilters(d.getSelection());
    }
}

void Split::setFilters(const QList<QUuid> ids)
{
    this->view_->setFilters(ids);
    this->header_->updateChannelText();
}

QList<QUuid> Split::getFilters() const
{
    return this->view_->getFilterIds();
}

void Split::showSearch(bool singleChannel)
{
    auto *popup = new SearchPopup(this, this);
    popup->setAttribute(Qt::WA_DeleteOnClose);

    if (singleChannel)
    {
        popup->addChannel(this->getChannelView());
        popup->show();
        return;
    }

    // Pass every ChannelView for every Split across the main window's tabs to
    // the search popup.
    auto &notebook = getApp()->getWindows()->getMainWindow().getNotebook();
    bool addedAnyChannel = false;
    notebook.forEachSplit([&](Split *split) {
        if (split == nullptr)
        {
            return;
        }

        if (split->channel_.getType() == Channel::Type::TwitchAutomod)
        {
            return;
        }

        popup->addChannel(split->getChannelView());
        addedAnyChannel = true;
    });

    if (!addedAnyChannel)
    {
        popup->addChannel(this->getChannelView());
    }

    popup->show();
}

void Split::reconnect()
{
    this->getChannel()->reconnect();
}

void Split::dragEnterEvent(QDragEnterEvent *event)
{
    if (getSettings()->imageUploaderEnabled &&
        (event->mimeData()->hasImage() || event->mimeData()->hasUrls()))
    {
        event->acceptProposedAction();
    }
    else
    {
        BaseWidget::dragEnterEvent(event);
    }
}

void Split::dropEvent(QDropEvent *event)
{
    if (getSettings()->imageUploaderEnabled &&
        (event->mimeData()->hasImage() || event->mimeData()->hasUrls()))
    {
        this->input_->ui_.textEdit->imagePasted.invoke(event->mimeData());
    }
    else
    {
        BaseWidget::dropEvent(event);
    }
}

void Split::drag()
{
    auto *container = dynamic_cast<SplitContainer *>(this->parentWidget());
    if (!container)
    {
        qCWarning(chatterinoWidget) << "Attempted to initiate split drag "
                                       "without a container parent";
        return;
    }

    startDraggingSplit();

    auto originalLocation = container->releaseSplit(this);
    auto *drag = new QDrag(this);
    auto *mimeData = new QMimeData;

    mimeData->setData("chatterino/split", "xD");
    drag->setMimeData(mimeData);

    // drag->exec is a blocking action
    auto dragRes = drag->exec(Qt::MoveAction);
    if (dragRes != Qt::MoveAction || drag->target() == nullptr)
    {
        // The split wasn't dropped in a valid spot, return it to its original position
        container->insertSplit(this, {.position = originalLocation});
    }

    stopDraggingSplit();
}

void Split::setInputReply(const MessagePtr &reply,
                          std::weak_ptr<Channel> channel)
{
    this->input_->setReply(reply, std::move(channel));
}

void Split::unpause()
{
    this->view_->unpause(PauseReason::KeyboardModifier);
    this->view_->unpause(PauseReason::DoubleClick);
    // Mouse intentionally left out, we may still have the mouse over the split
}

}  // namespace chatterino

QDebug operator<<(QDebug dbg, const chatterino::Split &split)
{
    auto channel = split.getChannel();
    if (channel)
    {
        dbg.nospace() << "Split(" << (void *)&split
                      << ", channel:" << channel->getName() << ")";
    }
    else
    {
        dbg.nospace() << "Split(" << (void *)&split << ", no channel)";
    }

    return dbg;
}

QDebug operator<<(QDebug dbg, const chatterino::Split *split)
{
    if (split != nullptr)
    {
        return operator<<(dbg, *split);
    }

    dbg.nospace() << "Split(nullptr)";

    return dbg;
}

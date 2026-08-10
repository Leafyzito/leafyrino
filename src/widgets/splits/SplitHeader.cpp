// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/SplitHeader.hpp"

#include "Application.hpp"
#include "common/network/NetworkCommon.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/builtin/Misc.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/hotkeys/Hotkey.hpp"
#include "controllers/hotkeys/HotkeyCategory.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/notifications/NotificationController.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/youtube/YouTubeChannel.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/FormatTime.hpp"
#include "util/Helpers.hpp"
#include "util/LayoutHelper.hpp"
#include "util/MultiChannel.hpp"
#include "widgets/buttons/DrawnButton.hpp"
#include "widgets/buttons/FollowButton.hpp"
#include "widgets/buttons/LabelButton.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/dialogs/SettingsDialog.hpp"
#include "widgets/helper/CommonTexts.hpp"
#include "widgets/Label.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/TooltipWidget.hpp"

#include <QDrag>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>

#include <cmath>

using namespace Qt::StringLiterals;

namespace {

using namespace chatterino;

/// The width of the standard button.
constexpr const int BUTTON_WIDTH = 28;

/// The width of the "Add split" button.
///
/// This matches the scrollbar's full width.
constexpr const int ADD_SPLIT_BUTTON_WIDTH = 16;

// 5 minutes
constexpr const qint64 THUMBNAIL_MAX_AGE_MS = 5LL * 60 * 1000;

auto formatRoomModeUnclean(const TwitchChannel::RoomModes &modes) -> QString
{
    QString text;

    if (modes.r9k)
    {
        text += "unique, ";
    }
    if (modes.slowMode > 0)
    {
        text += QString("slow(%1), ").arg(localizeNumbers(modes.slowMode));
    }
    if (modes.emoteOnly)
    {
        text += "emote, ";
    }
    if (modes.submode)
    {
        text += "sub, ";
    }
    if (modes.followerOnly != -1)
    {
        if (modes.followerOnly != 0)
        {
            text += QString("follow(%1), ")
                        .arg(formatDurationExact(
                            std::chrono::minutes{modes.followerOnly}));
        }
        else
        {
            text += QString("follow, ");
        }
    }

    return text;
}

QString formatRoomModeUnclean(const KickChannel::RoomModes &modes)
{
    TwitchChannel::RoomModes twitch{
        .submode = modes.subscribersMode,
        .r9k = false,
        .emoteOnly = modes.emotesMode,
        .followerOnly = -1,
        .slowMode = 0,
    };
    if (modes.followersModeDuration)
    {
        twitch.followerOnly =
            static_cast<int>(modes.followersModeDuration->count());
    }
    if (modes.slowModeDuration)
    {
        twitch.slowMode = static_cast<int>(modes.slowModeDuration->count());
    }
    return formatRoomModeUnclean(twitch);
}

void cleanRoomModeText(QString &text, bool hasModRights)
{
    if (text.length() > 2)
    {
        text = text.mid(0, text.size() - 2);
    }

    if (!text.isEmpty())
    {
        static QRegularExpression commaReplacement("^(.+?, .+?,) (.+)$");

        auto match = commaReplacement.match(text);
        if (match.hasMatch())
        {
            text = match.captured(1) + '\n' + match.captured(2);
        }
    }

    if (text.isEmpty() && hasModRights)
    {
        text = "none";
    }
}

auto formatTooltip(const TwitchChannel::StreamStatus &s, QString thumbnail,
                   bool limitSize = false)
{
    auto title = [&s]() -> QString {
        if (s.title.isEmpty())
        {
            return QStringLiteral("");
        }

        return s.title.toHtmlEscaped() + "<br><br>";
    }();

    auto tooltip = [&]() -> QString {
        if (getSettings()->thumbnailSizeStream.getValue() == 0)
        {
            return QStringLiteral("");
        }

        if (thumbnail.isEmpty())
        {
            return QStringLiteral("Couldn't fetch thumbnail<br>");
        }

        QString sizeStr;
        if (limitSize)
        {
            auto height =
                std::min(getSettings()->thumbnailSizeStream.getValue(), 4) * 80;
            sizeStr =
                QStringLiteral(" height=\"") % QString::number(height) % '"';
        }

        return u"<img " % sizeStr % u" src=\"data:image/jpg;base64, " %
               thumbnail % u"\"><br>";
    }();

    auto game = [&s]() -> QString {
        if (s.game.isEmpty())
        {
            return QStringLiteral("");
        }

        return s.game.toHtmlEscaped() + "<br>";
    }();

    auto extraStreamData = [&s]() -> QString {
        if (getApp()->getStreamerMode()->isEnabled() &&
            getSettings()->streamerModeHideViewerCountAndDuration)
        {
            return QStringLiteral(
                "<span style=\"color: #808892;\">&lt;Streamer "
                "Mode&gt;</span>");
        }

        return QString("%1 for %2 with %3 viewers")
            .arg(s.rerun ? "Vod-casting" : "Live")
            .arg(s.uptime)
            .arg(localizeNumbers(s.viewerCount));
    }();

    return QString("<p style=\"text-align: center;\">" +  //
                   title +                                //
                   tooltip +                              //
                   game +                                 //
                   extraStreamData +                      //
                   "</p>"                                 //
    );
}

auto formatOfflineTooltip(const TwitchChannel::StreamStatus &s)
{
    return QString("<p style=\"text-align: center;\">Offline<br>%1</p>")
        .arg(s.title.toHtmlEscaped());
}

QString youtubeHeaderName(const YouTubeChannel::StreamData &s)
{
    if (getSettings()->youtubeSplitHeaderUseHandle && !s.handle.isEmpty())
    {
        return s.handle;
    }
    if (!s.displayName.isEmpty())
    {
        return s.displayName;
    }
    return s.handle;
}

QString formatYouTubeTooltip(const YouTubeChannel::StreamData &s,
                             QString thumbnail)
{
    auto preview = [&]() -> QString {
        if (getSettings()->thumbnailSizeStream.getValue() == 0)
        {
            return QStringLiteral("");
        }
        if (thumbnail.isEmpty())
        {
            return QStringLiteral("Couldn't fetch thumbnail<br>");
        }
        auto height =
            std::min(getSettings()->thumbnailSizeStream.getValue(), 4) * 80;
        QString sizeStr =
            QStringLiteral(" height=\"") % QString::number(height) % '"';
        return u"<img " % sizeStr % u" src=\"data:image/jpg;base64, " %
               thumbnail % u"\"><br>";
    }();

    auto username = [&]() -> QString {
        auto name = s.handle.isEmpty() ? s.displayName : s.handle;
        if (name.isEmpty())
        {
            return QStringLiteral("");
        }
        return name.toHtmlEscaped() + "<br>";
    }();

    auto title = [&]() -> QString {
        if (s.title.isEmpty())
        {
            return QStringLiteral("");
        }
        return s.title.toHtmlEscaped() + "<br>";
    }();

    auto viewers = [&]() -> QString {
        if (getApp()->getStreamerMode()->isEnabled() &&
            getSettings()->streamerModeHideViewerCountAndDuration)
        {
            return QStringLiteral(
                "<span style=\"color: #808892;\">&lt;Streamer "
                "Mode&gt;</span>");
        }
        if (s.viewerCount < 0)
        {
            return QStringLiteral("");
        }
        return QString("%1 viewers").arg(localizeNumbers(s.viewerCount));
    }();

    return QString("<p style=\"text-align: center;\">" % preview % username %
                   title % viewers % "</p>");
}

QString formatYouTubeOfflineTooltip(const YouTubeChannel::StreamData &s)
{
    return QString("<p style=\"text-align: center;\">Offline<br>%1</p>")
        .arg(youtubeHeaderName(s).toHtmlEscaped());
}

auto formatTitle(const TwitchChannel::StreamStatus &s, Settings &settings,
                 const std::vector<HelixMinimalUser> &sharedChatParticipants)
{
    auto title = QString();

    // live
    if (s.rerun)
    {
        title += " (rerun)";
    }
    else if (s.streamType.isEmpty())
    {
        title += " (" + s.streamType + ")";
    }
    else
    {
        if (sharedChatParticipants.empty())
        {
            title += " (live)";
        }
        else
        {
            const auto mode = getSettings()->usernameDisplayMode.getEnum();
            QStringList names;
            for (const auto &p : sharedChatParticipants)
            {
                auto name = p.formatted(mode);
                names.push_back(std::move(name));
            }

            title += " (live with " + names.join(", ") + ")";
        }
    }

    // description
    if (settings.headerUptime)
    {
        title += " - " + s.uptime;
    }
    if (settings.headerViewerCount)
    {
        title += " - " + localizeNumbers(s.viewerCount);
    }
    if (settings.headerGame && !s.game.isEmpty())
    {
        title += " - " + s.game;
    }
    if (settings.headerStreamTitle && !s.title.isEmpty())
    {
        title += " - " + s.title.simplified();
    }

    return title;
}

TwitchChannel::StreamStatus toTwitchStreamStatus(
    const KickChannel::StreamData &data)
{
    return {
        .live = data.isLive,
        .viewerCount = static_cast<unsigned>(data.viewerCount),
        .title = data.title,
        .game = data.category,
        .uptime = data.uptime,
        .streamType = QStringLiteral("live"),
    };
}

struct MultiViewerSum {
    qint64 sum = 0;
    bool anyLive = false;
};

MultiViewerSum sumMultichannelViewers(const MultiChannel &mc)
{
    MultiViewerSum result;
    for (const auto &child : mc.channels())
    {
        auto *chan = child.channel.get();
        if (auto *tc = dynamic_cast<TwitchChannel *>(chan))
        {
            const auto status = tc->accessStreamStatus();
            if (status->live)
            {
                result.anyLive = true;
                result.sum += status->viewerCount;
            }
        }
        else if (auto *kc = dynamic_cast<KickChannel *>(chan))
        {
            const auto &stream = kc->streamData();
            if (stream.isLive)
            {
                result.anyLive = true;
                result.sum += static_cast<qint64>(stream.viewerCount);
            }
        }
        else if (auto *yt = dynamic_cast<YouTubeChannel *>(chan))
        {
            const auto &stream = yt->streamData();
            if (stream.isLive)
            {
                result.anyLive = true;
                if (stream.viewerCount > 0)
                {
                    result.sum += stream.viewerCount;
                }
            }
        }
    }
    return result;
}

auto distance(QPoint a, QPoint b)
{
    auto x = std::abs(a.x() - b.x());
    auto y = std::abs(a.y() - b.y());

    return std::sqrt(x * x + y * y);
}

}  // namespace

namespace chatterino {

SplitHeader::SplitHeader(Split *split)
    : BaseWidget(split)
    , split_(split)
    , tooltipWidget_(new TooltipWidget(this))
{
    this->initializeLayout();

    this->setMouseTracking(true);
    this->updateChannelText();
    this->handleChannelChanged();
    this->updateIcons();

    // The lifetime of these signals are tied to the lifetime of the Split.
    // Since the SplitHeader is owned by the Split, they will always be destroyed
    // at the same time.
    std::ignore = this->split_->focused.connect([this]() {
        this->themeChangedEvent();
    });
    std::ignore = this->split_->focusLost.connect([this]() {
        this->themeChangedEvent();
    });
    std::ignore = this->split_->channelChanged.connect([this]() {
        this->handleChannelChanged();
    });

    this->bSignals_.emplace_back(
        getApp()->getAccounts()->twitch.currentUserChanged.connect([this] {
            if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(
                    this->split_->getSelectedChannel().get());
                twitchChannel != nullptr && !twitchChannel->isEmpty() &&
                getSettings()->showFollowButtonInSplitHeader &&
                canUseFollowButtonForChannel(*twitchChannel))
            {
                twitchChannel->refreshFollowingStatus(false);
            }
            this->updateIcons();
        }));

    auto _ = [this](const auto &, const auto &) {
        this->updateChannelText();
    };
    getSettings()->headerViewerCount.connect(_, this->managedConnections_);
    getSettings()->headerStreamTitle.connect(_, this->managedConnections_);
    getSettings()->headerGame.connect(_, this->managedConnections_);
    getSettings()->headerUptime.connect(_, this->managedConnections_);
    getSettings()->showFollowButtonInSplitHeader.connect(
        [this](bool enabled, auto) {
            if (enabled)
            {
                if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(
                        this->split_->getSelectedChannel().get()))
                {
                    if (canUseFollowButtonForChannel(*twitchChannel))
                    {
                        twitchChannel->refreshFollowingStatus(false);
                    }
                }
            }
            this->updateIcons();
        },
        this->managedConnections_);
    getSettings()->moltorinoAuthAccounts.connect(
        [this](const QString &, auto) {
            if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(
                    this->split_->getSelectedChannel().get());
                twitchChannel != nullptr && !twitchChannel->isEmpty() &&
                getSettings()->showFollowButtonInSplitHeader &&
                canUseFollowButtonForChannel(*twitchChannel))
            {
                twitchChannel->refreshFollowingStatus(true);
            }
            this->updateIcons();
        },
        this->managedConnections_);

    auto *window = dynamic_cast<BaseWindow *>(this->window());
    if (window)
    {
        // Hack: In some cases Qt doesn't send the leaveEvent the "actual" last mouse receiver.
        // This can happen when quickly moving the mouse out of the window and right clicking.
        // To prevent the tooltip from getting stuck, we use the window's leaveEvent.
        this->managedConnections_.managedConnect(window->leaving, [this] {
            if (this->tooltipWidget_->isVisible())
            {
                this->tooltipWidget_->hide();
            }
        });
    }

    this->scaleChangedEvent(this->scale());
}

void SplitHeader::initializeLayout()
{
    assert(this->layout() == nullptr);

    this->moderationButton_ = new SvgButton(
        {
            .dark = ":/buttons/moderationDisabled-darkMode.svg",
            .light = ":/buttons/moderationDisabled-lightMode.svg",
        },
        this, {5, 5});

    this->chattersButton_ = new SvgButton(
        {
            .dark = ":/buttons/chatters-darkMode.svg",
            .light = ":/buttons/chatters-lightMode.svg",
        },
        this, {4, 4});

    this->youtubeRefreshButton_ = new SvgButton(
        {
            .dark = ":/buttons/reloadLight.svg",
            .light = ":/buttons/reloadDark.svg",
        },
        this, {4, 4});
    this->youtubeRefreshButton_->setToolTip(
        "Refresh YouTube live chat (find the channel's latest stream)");
    this->youtubeRefreshButton_->hide();

    this->sendTargetButton_ = new LabelButton({}, this);
    this->sendTargetButton_->setSizePolicy(QSizePolicy::Minimum,
                                           QSizePolicy::Minimum);
    this->sendTargetButton_->hide();

    this->followButton_ =
        new SvgButton(followButtonSource(false), this, {4, 4});

    this->addButton_ = new DrawnButton(DrawnButton::Symbol::Plus,
                                       {
                                           .padding = 3,
                                           .thickness = 1,
                                       },
                                       this);

    this->dropdownButton_ =
        new DrawnButton(DrawnButton::Symbol::Kebab, {}, this);

    /// XXX: this never gets disconnected
    QObject::connect(this->dropdownButton_, &Button::leftMousePress, this,
                     [this] {
                         this->dropdownButton_->setMenu(this->createMainMenu());
                     });

    auto *layout = makeLayout<QHBoxLayout>({
        // follow
        this->followButton_,
        // space
        makeWidget<BaseWidget>([](auto w) {
            w->setScaleIndependentSize(8, 4);
        }),
        // title
        this->titleLabel_ = makeWidget<Label>([](auto w) {
            w->setSizePolicy(QSizePolicy::MinimumExpanding,
                             QSizePolicy::Preferred);
            w->setCentered(true);
            w->setPadding(QMargins{});
        }),
        // space
        makeWidget<BaseWidget>([](auto w) {
            w->setScaleIndependentSize(8, 4);
        }),
        // mode
        this->modeButton_ = makeWidget<LabelButton>([&](auto w) {
            w->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
            w->hide();
            w->setMenu(this->createChatModeMenu());
        }),
        // moderator
        this->moderationButton_,
        // chatter list
        this->chattersButton_,
        this->sendTargetButton_,
        this->youtubeRefreshButton_,
        // dropdown
        this->dropdownButton_,
        // add split
        this->addButton_,
    });

    QObject::connect(
        this->moderationButton_, &Button::clicked, this,
        [this](Qt::MouseButton button) mutable {
            switch (button)
            {
                case Qt::LeftButton:
                    if (getSettings()->moderationActions.empty())
                    {
                        getApp()->getWindows()->showSettingsDialog(
                            this, SettingsDialogPreference::ModerationActions);
                        this->split_->setModerationMode(true);
                    }
                    else
                    {
                        auto moderationMode = this->split_->getModerationMode();

                        this->split_->setModerationMode(!moderationMode);
                        // w->setDim(moderationMode ? DimButton::Dim::Some
                        //                          : DimButton::Dim::None);
                    }
                    break;

                case Qt::RightButton:
                case Qt::MiddleButton:
                    getApp()->getWindows()->showSettingsDialog(
                        this, SettingsDialogPreference::ModerationActions);
                    break;

                default:
                    break;
            }
        });

    QObject::connect(this->chattersButton_, &Button::leftClicked, this,
                     [this]() {
                         this->split_->openChatterList();
                     });

    QObject::connect(
        this->youtubeRefreshButton_, &Button::leftClicked, this, [this]() {
            auto channel = this->split_->getChannel();
            if (auto *yt = dynamic_cast<YouTubeChannel *>(channel.get()))
            {
                yt->refreshLiveStream();
            }
            else if (auto *mc = dynamic_cast<MultiChannel *>(channel.get()))
            {
                for (const auto &child : mc->channels())
                {
                    if (auto *ytc =
                            dynamic_cast<YouTubeChannel *>(child.channel.get()))
                    {
                        ytc->refreshLiveStream();
                    }
                }
            }
        });

    QObject::connect(this->sendTargetButton_, &Button::leftClicked, this,
                     [this]() {
                         this->cycleSendTarget();
                     });

    QObject::connect(this->followButton_, &Button::leftClicked, this, [this]() {
        this->toggleFollow();
    });

    QObject::connect(this->addButton_, &Button::leftClicked, this, [this]() {
        this->split_->addSibling();
    });

    getSettings()->customURIScheme.connect(
        [this] {
            if (auto *const drop = this->dropdownButton_)
            {
                drop->setMenu(this->createMainMenu());
            }
        },
        this->managedConnections_);

    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    this->setLayout(layout);

    this->setAddButtonVisible(false);
}

std::unique_ptr<QMenu> SplitHeader::createMainMenu()
{
    // top level menu
    const auto &h = getApp()->getHotkeys();
    auto menu = std::make_unique<QMenu>();
    menu->setToolTipsVisible(true);

    auto selected = this->split_->getSelectedChannel();
    auto *twitchChannel = dynamic_cast<TwitchChannel *>(selected.get());
    auto *kickChannel = dynamic_cast<KickChannel *>(selected.get());

    menu->addAction(
        "Change channel",
        h->getDisplaySequence(HotkeyCategory::Split, "changeChannel"),
        this->split_, &Split::changeChannel);
    menu->addAction("Close",
                    h->getDisplaySequence(HotkeyCategory::Split, "delete"),
                    this->split_, &Split::deleteFromContainer);
    menu->addSeparator();
    menu->addAction(
        "Popup",
        h->getDisplaySequence(HotkeyCategory::Window, "popup", {{"split"}}),
        this->split_, &Split::popup);
    menu->addAction(
        "Popup overlay",
        h->getDisplaySequence(HotkeyCategory::Split, "popupOverlay"),
        this->split_, &Split::showOverlayWindow);
    menu->addAction(u"Search…"_s,
                    h->getDisplaySequence(HotkeyCategory::Split, "showSearch"),
                    this->split_, [this] {
                        this->split_->showSearch(true);
                    });
    menu->addAction(
        "Search all open tabs",
        h->getDisplaySequence(HotkeyCategory::Split, "showGlobalSearch"),
        this->split_, [this] {
            this->split_->showSearch(false);
        });
    menu->addAction(u"Set filters…"_s,
                    h->getDisplaySequence(HotkeyCategory::Split, "pickFilters"),
                    this->split_, &Split::setFiltersDialog);

    if (twitchChannel)
    {
        menu->addSeparator();

        auto *bannersMenu =
            menu->addMenu(QStringLiteral("Banners in this split"));
        const bool hideAll = this->split_->perSplitHidePinnedMessage() &&
                             this->split_->perSplitHidePrediction() &&
                             this->split_->perSplitHidePoll();
        auto *hideAllBannersAction =
            bannersMenu->addAction(QStringLiteral("Hide all"));
        hideAllBannersAction->setCheckable(true);
        hideAllBannersAction->setChecked(hideAll);
        QObject::connect(hideAllBannersAction, &QAction::toggled, this->split_,
                         &Split::setPerSplitHideAllBanners);
        bannersMenu->addSeparator();
        auto *hidePinnedAction =
            bannersMenu->addAction(QStringLiteral("Hide pinned message"));
        hidePinnedAction->setCheckable(true);
        hidePinnedAction->setChecked(this->split_->perSplitHidePinnedMessage());
        QObject::connect(hidePinnedAction, &QAction::toggled, this->split_,
                         &Split::setPerSplitHidePinnedMessage);
        auto *hidePredictionAction =
            bannersMenu->addAction(QStringLiteral("Hide prediction"));
        hidePredictionAction->setCheckable(true);
        hidePredictionAction->setChecked(
            this->split_->perSplitHidePrediction());
        QObject::connect(hidePredictionAction, &QAction::toggled, this->split_,
                         &Split::setPerSplitHidePrediction);
        auto *hidePollAction =
            bannersMenu->addAction(QStringLiteral("Hide poll"));
        hidePollAction->setCheckable(true);
        hidePollAction->setChecked(this->split_->perSplitHidePoll());
        QObject::connect(hidePollAction, &QAction::toggled, this->split_,
                         &Split::setPerSplitHidePoll);

        menu->addAction(QStringLiteral("Restore dismissed banners"),
                        this->split_, &Split::recoverDismissedBanners);
        menu->addSeparator();
    }
    else if (kickChannel)
    {
        menu->addSeparator();
    }

    if (twitchChannel || kickChannel)
    {
        menu->addAction(
            OPEN_IN_BROWSER,
            h->getDisplaySequence(HotkeyCategory::Split, "openInBrowser"),
            this->split_, &Split::openInBrowser);
        if (twitchChannel)
        {
            menu->addAction(OPEN_PLAYER_IN_BROWSER,
                            h->getDisplaySequence(HotkeyCategory::Split,
                                                  "openPlayerInBrowser"),
                            this->split_, &Split::openBrowserPlayer);
        }
        menu->addAction(
            OPEN_IN_STREAMLINK,
            h->getDisplaySequence(HotkeyCategory::Split, "openInStreamlink"),
            this->split_, &Split::openInStreamlink);

        if (!getSettings()->customURIScheme.getValue().isEmpty())
        {
            menu->addAction("Open in custom player",
                            h->getDisplaySequence(HotkeyCategory::Split,
                                                  "openInCustomPlayer"),
                            this->split_, &Split::openWithCustomScheme);
        }

        if (this->split_->getChannel()->hasModRights())
        {
            menu->addAction(
                OPEN_MOD_VIEW_IN_BROWSER,
                h->getDisplaySequence(HotkeyCategory::Split, "openModView"),
                this->split_, &Split::openModViewInBrowser);
        }

        if (twitchChannel)
        {
            menu->addAction(
                    "Create a clip",
                    h->getDisplaySequence(HotkeyCategory::Split, "createClip"),
                    this->split_,
                    [twitchChannel] {
                        twitchChannel->createClip({}, {});
                    })
                ->setVisible(twitchChannel->isLive());
        }

        if (this->split_->getIndirectChannel().getType() ==
            Channel::Type::TwitchWatching)
        {
            menu->addAction("Reset /watching", this->split_, [] {
                if (!getApp()
                         ->getTwitch()
                         ->getWatchingChannel()
                         .get()
                         ->isEmpty())
                {
                    getApp()->getTwitch()->setWatchingChannel(
                        Channel::getEmpty());
                }
            });
        }

        menu->addSeparator();
    }

    if (this->split_->getSelectedChannel()->getType() ==
        Channel::Type::TwitchWhispers)
    {
        menu->addAction(
            OPEN_WHISPERS_IN_BROWSER,
            h->getDisplaySequence(HotkeyCategory::Split, "openInBrowser"),
            this->split_, &Split::openWhispersInBrowser);
        menu->addSeparator();
    }

    // reload / reconnect
    if (this->split_->getChannel()->canReconnect())
    {
        menu->addAction(
            "Reconnect",
            h->getDisplaySequence(HotkeyCategory::Split, "reconnect"), this,
            &SplitHeader::reconnect);
    }

    if (twitchChannel || kickChannel)
    {
        auto bothSeq = h->getDisplaySequence(
            HotkeyCategory::Split, "reloadEmotes", {std::vector<QString>()});
        auto channelSeq = h->getDisplaySequence(HotkeyCategory::Split,
                                                "reloadEmotes", {{"channel"}});
        auto subSeq = h->getDisplaySequence(HotkeyCategory::Split,
                                            "reloadEmotes", {{"subscriber"}});
        menu->addAction("Reload channel emotes",
                        channelSeq.isEmpty() ? bothSeq : channelSeq, this,
                        &SplitHeader::reloadChannelEmotes);
        if (twitchChannel)
        {
            menu->addAction("Reload subscriber emotes",
                            subSeq.isEmpty() ? bothSeq : subSeq, this,
                            &SplitHeader::reloadSubscriberEmotes);
        }
    }

    if ((twitchChannel || kickChannel) && !selected->isEmpty())
    {
        menu->addSeparator();

        const auto selectedChannelName =
            this->split_->getSelectedChannel()->getName();
        auto *autoTranslateAction = new QAction(menu.get());
        autoTranslateAction->setText("Auto-translate messages (risky)");
        autoTranslateAction->setCheckable(true);
        autoTranslateAction->setToolTip(
            "Translates new visible chat messages in this channel as they "
            "arrive. Risky in fast chats because translation requests can hit "
            "rate limits, fail, or skip messages.");
        autoTranslateAction->setStatusTip(autoTranslateAction->toolTip());

        QObject::connect(
            menu.get(), &QMenu::aboutToShow, this,
            [autoTranslateAction, selectedChannelName]() {
                autoTranslateAction->setChecked(
                    getSettings()->isAutoTranslateChannel(selectedChannelName));
            });
        QObject::connect(autoTranslateAction, &QAction::triggered, this,
                         [autoTranslateAction, selectedChannelName]() {
                             autoTranslateAction->setChecked(
                                 getSettings()->toggleAutoTranslateChannel(
                                     selectedChannelName));
                         });

        menu->addAction(autoTranslateAction);
    }

    menu->addSeparator();

    {
        // "How to..." sub menu
        auto *subMenu = new QMenu("How to...", this);
        subMenu->addAction("move split", this->split_, &Split::explainMoving);
        subMenu->addAction("add/split", this->split_, &Split::explainSplitting);
        menu->addMenu(subMenu);
    }

    menu->addSeparator();

    // sub menu
    auto *moreMenu = new QMenu("Mor&e", this);

    auto modModeSeq = h->getDisplaySequence(HotkeyCategory::Split,
                                            "setModerationMode", {{"toggle"}});
    if (modModeSeq.isEmpty())
    {
        modModeSeq =
            h->getDisplaySequence(HotkeyCategory::Split, "setModerationMode",
                                  {std::vector<QString>()});
        // this makes a full std::optional<> with an empty vector inside
    }
    moreMenu->addAction(
        "&Toggle moderation mode", modModeSeq, this->split_, [this]() {
            this->split_->setModerationMode(!this->split_->getModerationMode());
        });

    if (this->split_->getChannel()->getType() == Channel::Type::TwitchMentions)
    {
        auto *action = new QAction(this);
        action->setText("Enable /mention tab highlights");
        action->setCheckable(true);

        QObject::connect(moreMenu, &QMenu::aboutToShow, this, [action]() {
            action->setChecked(getSettings()->highlightMentions);
        });
        QObject::connect(action, &QAction::triggered, this, []() {
            getSettings()->highlightMentions =
                !getSettings()->highlightMentions;
        });

        moreMenu->addAction(action);
    }

    if (twitchChannel)
    {
        if (twitchChannel->hasModRights())
        {
            moreMenu->addAction(
                "Show chatter &list",
                h->getDisplaySequence(HotkeyCategory::Split, "openViewerList"),
                this->split_, &Split::openChatterList);
        }

        moreMenu->addAction("&Subscribe",
                            h->getDisplaySequence(HotkeyCategory::Split,
                                                  "openSubscriptionPage"),
                            this->split_, &Split::openSubPage);

        {
            auto *action = new QAction(this);
            action->setText("&Notify when live");
            action->setCheckable(true);

            auto notifySeq = h->getDisplaySequence(
                HotkeyCategory::Split, "setChannelNotification", {{"toggle"}});
            if (notifySeq.isEmpty())
            {
                notifySeq = h->getDisplaySequence(HotkeyCategory::Split,
                                                  "setChannelNotification",
                                                  {std::vector<QString>()});
                // this makes a full std::optional<> with an empty vector inside
            }
            action->setShortcut(notifySeq);

            QObject::connect(
                moreMenu, &QMenu::aboutToShow, this, [action, this]() {
                    action->setChecked(
                        getApp()->getNotifications()->isChannelNotified(
                            this->split_->getSelectedChannel()->getName(),
                            Platform::Twitch));
                });
            QObject::connect(action, &QAction::triggered, this, [this]() {
                getApp()->getNotifications()->updateChannelNotification(
                    this->split_->getSelectedChannel()->getName(),
                    Platform::Twitch);
            });

            moreMenu->addAction(action);
        }

        {
            auto *action = new QAction(this);
            action->setText("&Mute highlight sounds");
            action->setCheckable(true);

            auto notifySeq = h->getDisplaySequence(
                HotkeyCategory::Split, "setHighlightSounds", {{"toggle"}});
            if (notifySeq.isEmpty())
            {
                notifySeq = h->getDisplaySequence(HotkeyCategory::Split,
                                                  "setHighlightSounds",
                                                  {std::vector<QString>()});
            }
            action->setShortcut(notifySeq);

            QObject::connect(
                moreMenu, &QMenu::aboutToShow, this, [action, this]() {
                    action->setChecked(getSettings()->isMutedChannel(
                        this->split_->getSelectedChannel()->getName()));
                });
            QObject::connect(action, &QAction::triggered, this, [this]() {
                getSettings()->toggleMutedChannel(
                    this->split_->getSelectedChannel()->getName());
            });

            moreMenu->addAction(action);
        }
    }

    moreMenu->addSeparator();
    moreMenu->addAction(
        "&Clear messages",
        h->getDisplaySequence(HotkeyCategory::Split, "clearMessages"),
        this->split_, &Split::clear);
    //    moreMenu->addSeparator();
    //    moreMenu->addAction("Show changelog", this,
    //    SLOT(moreMenuShowChangelog()));
    menu->addMenu(moreMenu);

    return menu;
}

std::unique_ptr<QMenu> SplitHeader::createChatModeMenu()
{
    auto menu = std::make_unique<QMenu>();

    this->modeActionSetSub = new QAction("Subscriber only", this);
    this->modeActionSetEmote = new QAction("Emote only", this);
    this->modeActionSetSlow = new QAction("Slow", this);
    this->modeActionSetR9k = new QAction("Unique chat (R9K)", this);
    this->modeActionSetFollowers = new QAction("Followers only", this);

    this->modeActionSetFollowers->setCheckable(true);
    this->modeActionSetSub->setCheckable(true);
    this->modeActionSetEmote->setCheckable(true);
    this->modeActionSetSlow->setCheckable(true);
    this->modeActionSetR9k->setCheckable(true);

    menu->addAction(this->modeActionSetEmote);
    menu->addAction(this->modeActionSetSub);
    menu->addAction(this->modeActionSetSlow);
    menu->addAction(this->modeActionSetR9k);
    menu->addAction(this->modeActionSetFollowers);

    auto execCommand = [this](const QString &command) {
        auto channel = this->split_->getSelectedChannel();
        auto text =
            getApp()->getCommands()->execCommand(command, channel, false);
        channel->sendMessage(text);
    };
    auto toggle = [execCommand](const QString &command,
                                QAction *action) mutable {
        execCommand(command + (action->isChecked() ? "" : "off"));
        action->setChecked(!action->isChecked());
    };

    QObject::connect(this->modeActionSetSub, &QAction::triggered, this,
                     [this, toggle]() mutable {
                         toggle("/subscribers", this->modeActionSetSub);
                     });

    QObject::connect(this->modeActionSetEmote, &QAction::triggered, this,
                     [this, toggle]() mutable {
                         toggle("/emoteonly", this->modeActionSetEmote);
                     });

    QObject::connect(this->modeActionSetSlow, &QAction::triggered, this,
                     [this, execCommand]() {
                         if (!this->modeActionSetSlow->isChecked())
                         {
                             execCommand("/slowoff");
                             this->modeActionSetSlow->setChecked(false);
                             return;
                         };
                         auto ok = bool();
                         auto seconds = QInputDialog::getInt(
                             this, "", "Seconds:", 10, 0, 500, 1, &ok,
                             Qt::FramelessWindowHint);
                         if (ok)
                         {
                             execCommand(QString("/slow %1").arg(seconds));
                         }
                         else
                         {
                             this->modeActionSetSlow->setChecked(false);
                         }
                     });

    QObject::connect(this->modeActionSetFollowers, &QAction::triggered, this,
                     [this, execCommand]() {
                         if (!this->modeActionSetFollowers->isChecked())
                         {
                             execCommand("/followersoff");
                             this->modeActionSetFollowers->setChecked(false);
                             return;
                         };
                         auto ok = bool();
                         auto time = QInputDialog::getText(
                             this, "", "Time:", QLineEdit::Normal, "15m", &ok,
                             Qt::FramelessWindowHint,
                             Qt::ImhLowercaseOnly | Qt::ImhPreferNumbers);
                         if (ok)
                         {
                             execCommand(QString("/followers %1").arg(time));
                         }
                         else
                         {
                             this->modeActionSetFollowers->setChecked(false);
                         }
                     });

    QObject::connect(this->modeActionSetR9k, &QAction::triggered, this,
                     [this, toggle]() mutable {
                         toggle("/r9kbeta", this->modeActionSetR9k);
                     });

    return menu;
}

void SplitHeader::updateRoomModes()
{
    assert(this->modeButton_ != nullptr);
    auto chan = this->split_->getSelectedChannel();

    // Update the mode button
    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(chan.get()))
    {
        this->modeButton_->setEnabled(twitchChannel->hasModRights());

        QString text;
        {
            auto roomModes = twitchChannel->accessRoomModes();
            text = formatRoomModeUnclean(*roomModes);

            // Set menu action
            this->modeActionSetR9k->setChecked(roomModes->r9k);
            this->modeActionSetSlow->setChecked(roomModes->slowMode > 0);
            this->modeActionSetEmote->setChecked(roomModes->emoteOnly);
            this->modeActionSetSub->setChecked(roomModes->submode);
            this->modeActionSetFollowers->setChecked(roomModes->followerOnly !=
                                                     -1);
        }
        cleanRoomModeText(text, twitchChannel->hasModRights());

        // set the label text

        if (!text.isEmpty())
        {
            this->modeButton_->setText(text);
            this->modeButton_->show();
        }
        else
        {
            this->modeButton_->hide();
        }

        // Update the mode button menu actions
    }
    else if (auto *kc = dynamic_cast<KickChannel *>(chan.get()))
    {
        this->modeButton_->setEnabled(false);

        QString text = formatRoomModeUnclean(kc->roomModes());
        cleanRoomModeText(text, false);

        if (!text.isEmpty())
        {
            this->modeButton_->setText(text);
            this->modeButton_->show();
        }
        else
        {
            this->modeButton_->hide();
        }
    }
    else
    {
        this->modeButton_->hide();
    }
}

void SplitHeader::resetThumbnail()
{
    this->lastThumbnail_.invalidate();
    this->thumbnail_.clear();
}

void SplitHeader::handleChannelChanged()
{
    this->resetThumbnail();

    this->updateChannelText();

    this->channelConnections_.clear();
    auto channel = this->split_->getChannel();
    if (auto *multiChannel = dynamic_cast<MultiChannel *>(channel.get()))
    {
        this->channelConnections_.managedConnect(
            multiChannel->activeChannelChanged, [this] {
                this->handleChannelChanged();
                this->updateIcons();
                this->updateRoomModes();
            });
        for (const auto &child : multiChannel->channels())
        {
            auto *chan = child.channel.get();
            if (auto *tc = dynamic_cast<TwitchChannel *>(chan))
            {
                this->channelConnections_.managedConnect(
                    tc->streamStatusChanged, [this]() {
                        this->updateChannelText();
                    });
            }
            else if (auto *kc = dynamic_cast<KickChannel *>(chan))
            {
                this->channelConnections_.managedConnect(
                    kc->streamDataChanged, [this]() {
                        this->updateChannelText();
                    });
            }
            else if (auto *yt = dynamic_cast<YouTubeChannel *>(chan))
            {
                this->channelConnections_.managedConnect(
                    yt->streamDataChanged, [this]() {
                        this->updateChannelText();
                    });
            }
        }
        if (const auto *active = multiChannel->activeChannel())
        {
            channel = active->channel;
        }
    }
    else if (auto *kickChannel = dynamic_cast<KickChannel *>(channel.get()))
    {
        this->channelConnections_.managedConnect(kickChannel->streamDataChanged,
                                                 [this]() {
                                                     this->updateChannelText();
                                                 });
    }
    else if (auto *youtubeChannel =
                 dynamic_cast<YouTubeChannel *>(channel.get()))
    {
        this->channelConnections_.managedConnect(
            youtubeChannel->streamDataChanged, [this]() {
                this->updateChannelText();
            });
    }

    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        this->channelConnections_.managedConnect(
            twitchChannel->streamStatusChanged, [this]() {
                this->updateChannelText();
            });
        this->channelConnections_.managedConnect(
            twitchChannel->followingStatusChanged, [this]() {
                this->updateIcons();
            });
        if (getSettings()->showFollowButtonInSplitHeader &&
            canUseFollowButtonForChannel(*twitchChannel))
        {
            twitchChannel->refreshFollowingStatus(false);
        }
    }

    auto splitChannel = this->split_->getChannel();
    bool hasYouTube =
        dynamic_cast<YouTubeChannel *>(splitChannel.get()) != nullptr;
    if (!hasYouTube)
    {
        if (auto *mc = dynamic_cast<MultiChannel *>(splitChannel.get()))
        {
            for (const auto &child : mc->channels())
            {
                if (dynamic_cast<YouTubeChannel *>(child.channel.get()))
                {
                    hasYouTube = true;
                    break;
                }
            }
        }
    }
    if (this->youtubeRefreshButton_)
    {
        this->youtubeRefreshButton_->setVisible(hasYouTube);
    }

    this->updateSendTargetButton();
}

namespace {

QString multiPlatformName(MultiChannel::Platform platform)
{
    switch (platform)
    {
        case MultiChannel::Platform::Twitch:
            return u"Twitch"_s;
        case MultiChannel::Platform::Kick:
            return u"Kick"_s;
        case MultiChannel::Platform::YouTube:
            return u"YouTube"_s;
    }
    return {};
}

}  // namespace

void SplitHeader::updateSendTargetButton()
{
    if (!this->sendTargetButton_)
    {
        return;
    }

    auto channel = this->split_->getChannel();
    auto *mc = dynamic_cast<MultiChannel *>(channel.get());
    if (!mc)
    {
        this->sendTargetButton_->hide();
        return;
    }

    auto channels = mc->channels();
    int writable = 0;
    for (const auto &child : channels)
    {
        if (child.channel->isWritable())
        {
            writable++;
        }
    }
    if (writable < 2)
    {
        this->sendTargetButton_->hide();
        return;
    }

    const auto *active = mc->activeChannel();
    if (!active)
    {
        this->sendTargetButton_->hide();
        return;
    }

    const auto name = active->channel->getName();
    const auto platform = multiPlatformName(active->platform);
    this->sendTargetButton_->setText(name);
    this->sendTargetButton_->setToolTip(u"Sending to: " % name % u" (" %
                                        platform % u"). Click to change.");
    this->sendTargetButton_->show();
}

void SplitHeader::cycleSendTarget()
{
    auto channel = this->split_->getChannel();
    auto *mc = dynamic_cast<MultiChannel *>(channel.get());
    if (!mc)
    {
        return;
    }

    auto channels = mc->channels();
    const size_t count = channels.size();
    if (count == 0)
    {
        return;
    }

    const size_t current = mc->activeChannelIndex();
    for (size_t step = 1; step <= count; step++)
    {
        const size_t index = (current + step) % count;
        if (channels[index].channel->isWritable())
        {
            mc->setActiveChannelIndex(index);
            getApp()->getWindows()->forceLayoutChannelViews();
            break;
        }
    }
}

void SplitHeader::scaleChangedEvent(float scale)
{
    int w = int(BUTTON_WIDTH * scale);
    int addSplitWidth = int(ADD_SPLIT_BUTTON_WIDTH * scale);

    this->setFixedHeight(w);
    this->dropdownButton_->setFixedWidth(w);
    this->followButton_->setFixedWidth(w);
    this->moderationButton_->setFixedWidth(w);
    this->chattersButton_->setFixedWidth(w);
    this->youtubeRefreshButton_->setFixedWidth(w);

    this->addButton_->setFixedWidth(addSplitWidth);
}

void SplitHeader::setAddButtonVisible(bool value)
{
    this->addButton_->setVisible(value);
}

void SplitHeader::toggleFollow()
{
    auto channel = this->split_->getSelectedChannel();
    auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get());
    if (twitchChannel == nullptr || twitchChannel->isEmpty())
    {
        return;
    }

    const auto command =
        twitchChannel->isFollowingStatusKnown() && twitchChannel->isFollowing()
            ? QStringLiteral("/unfollow")
            : QStringLiteral("/follow");

    if (command == QStringLiteral("/unfollow") &&
        getSettings()->confirmUnfollowFromSplitHeader)
    {
        const auto displayName = channel->getLocalizedName().isEmpty()
                                     ? channel->getName()
                                     : channel->getLocalizedName();

        QMessageBox box(this);
        box.setWindowTitle("Unfollow channel?");
        box.setIcon(QMessageBox::Question);
        box.setText(
            QString("Are you sure you want to unfollow %1?").arg(displayName));

        auto *confirmButton =
            box.addButton("Confirm", QMessageBox::DestructiveRole);
        auto *cancelButton = box.addButton("Cancel", QMessageBox::RejectRole);
        box.setDefaultButton(cancelButton);
        box.setEscapeButton(cancelButton);
        box.exec();

        if (box.clickedButton() != confirmButton)
        {
            return;
        }
    }

    CommandContext ctx{
        .words = {command},
        .rawText = command,
        .channel = channel,
        .twitchChannel = twitchChannel,
        .kickChannel = nullptr,
    };
    const auto text = command == QStringLiteral("/unfollow")
                          ? commands::unfollow(ctx)
                          : commands::follow(ctx);
    if (!text.isEmpty())
    {
        channel->sendMessage(text);
    }
}

void SplitHeader::updateChannelText()
{
    auto indirectChannel = this->split_->getIndirectChannel();
    this->isLive_ = false;
    this->tooltipText_ = QString();

    if (auto *mc =
            dynamic_cast<MultiChannel *>(this->split_->getChannel().get()))
    {
        QString mainText;
        bool first = true;
        for (const auto &child : mc->channels())
        {
            if (!first)
            {
                mainText += ", ";
            }
            first = false;
            mainText += child.channel->getLocalizedName();
        }

        auto viewers = sumMultichannelViewers(*mc);
        this->isLive_ = viewers.anyLive;

        QString suffix;
        if (viewers.anyLive)
        {
            suffix += " (live)";
            if (mc->combinedViewerCount())
            {
                suffix += " - " + localizeNumbers(viewers.sum);
            }
        }
        if (!mainText.isEmpty() && !this->split_->getFilters().empty())
        {
            suffix += " - filtered";
        }

        this->titleLabel_->setShouldElide(true);
        this->titleLabel_->setElideSuffix(suffix);
        this->titleLabel_->setText(mainText.isEmpty() ? "<empty>" : mainText);
        return;
    }

    this->titleLabel_->setShouldElide(false);
    this->titleLabel_->setElideSuffix(QString());

    auto selectedChannel = this->split_->getSelectedChannel();

    auto title = selectedChannel->getLocalizedName();

    if (indirectChannel.getType() == Channel::Type::TwitchWatching)
    {
        title = "watching: " + (title.isEmpty() ? "none" : title);
    }

    if (auto *twitchChannel =
            dynamic_cast<TwitchChannel *>(selectedChannel.get()))
    {
        if (twitchChannel->isAnonymous() && !title.isEmpty())
        {
            title += " (anonymous)";
        }

        const auto streamStatus = twitchChannel->accessStreamStatus();

        if (streamStatus->live)
        {
            this->isLive_ = true;
            // XXX: This URL format can be figured out from the Helix Get Streams API which we parse in TwitchChannel::parseLiveStatus
            QString url = "https://static-cdn.jtvnw.net/"
                          "previews-ttv/live_user_" +
                          selectedChannel->getName().toLower();
            switch (getSettings()->thumbnailSizeStream.getValue())
            {
                case 1:
                    url.append("-80x45.jpg");
                    break;
                case 2:
                    url.append("-160x90.jpg");
                    break;
                case 3:
                    url.append("-360x203.jpg");
                    break;
                default:
                    url = "";
            }
            if (!url.isEmpty() &&
                (!this->lastThumbnail_.isValid() ||
                 this->lastThumbnail_.elapsed() > THUMBNAIL_MAX_AGE_MS))
            {
                NetworkRequest(url, NetworkRequestType::Get)
                    .caller(this)
                    .onSuccess([this](auto result) {
                        assert(!isAppAboutToQuit());

                        // NOTE: We do not follow the redirects, so we need to make sure we only treat code 200 as a valid image
                        if (result.status() == 200)
                        {
                            this->thumbnail_ = QString::fromLatin1(
                                result.getData().toBase64());
                        }
                        else
                        {
                            this->thumbnail_.clear();
                        }
                        this->updateChannelText();
                    })
                    .execute();
                this->lastThumbnail_.restart();
            }
            this->tooltipText_ = formatTooltip(*streamStatus, this->thumbnail_);

            title +=
                formatTitle(*streamStatus, *getSettings(),
                            twitchChannel->getSharedChatSessionParticipants());
        }
        else
        {
            this->tooltipText_ = formatOfflineTooltip(*streamStatus);
        }
    }
    else if (auto *kickChannel =
                 dynamic_cast<KickChannel *>(selectedChannel.get()))
    {
        const auto &stream = kickChannel->streamData();
        auto twitch = toTwitchStreamStatus(stream);
        if (stream.isLive)
        {
            this->isLive_ = true;
            if (!stream.thumbnailUrl.isEmpty() &&
                (!this->lastThumbnail_.isValid() ||
                 this->lastThumbnail_.elapsed() > THUMBNAIL_MAX_AGE_MS))
            {
                NetworkRequest(stream.thumbnailUrl, NetworkRequestType::Get)
                    .caller(this)
                    .followRedirects(true)
                    .onSuccess([this](const auto &result) {
                        assert(!isAppAboutToQuit());

                        this->thumbnail_ =
                            QString::fromLatin1(result.getData().toBase64());
                        this->updateChannelText();
                    })
                    .execute();
                this->lastThumbnail_.restart();
            }
            this->tooltipText_ = formatTooltip(twitch, this->thumbnail_, true);
            title += formatTitle(twitch, *getSettings(), {});
        }
        else
        {
            this->tooltipText_ = formatOfflineTooltip(twitch);
        }
    }
    else if (auto *youtubeChannel =
                 dynamic_cast<YouTubeChannel *>(selectedChannel.get()))
    {
        const auto &stream = youtubeChannel->streamData();
        auto name = youtubeHeaderName(stream);
        if (!name.isEmpty())
        {
            title = name;
        }
        if (stream.isLive)
        {
            this->isLive_ = true;
            if (!stream.thumbnailUrl.isEmpty() &&
                (!this->lastThumbnail_.isValid() ||
                 this->lastThumbnail_.elapsed() > THUMBNAIL_MAX_AGE_MS))
            {
                NetworkRequest(stream.thumbnailUrl, NetworkRequestType::Get)
                    .caller(this)
                    .followRedirects(true)
                    .onSuccess([this](const auto &result) {
                        assert(!isAppAboutToQuit());

                        this->thumbnail_ =
                            QString::fromLatin1(result.getData().toBase64());
                        this->updateChannelText();
                    })
                    .execute();
                this->lastThumbnail_.restart();
            }
            this->tooltipText_ = formatYouTubeTooltip(stream, this->thumbnail_);

            title += " (live)";
            if (getSettings()->youtubeHeaderViewerCount &&
                stream.viewerCount >= 0)
            {
                title += " - " + localizeNumbers(stream.viewerCount);
            }
        }
        else
        {
            this->tooltipText_ = formatYouTubeOfflineTooltip(stream);
        }
    }

    if (!title.isEmpty() && !this->split_->getFilters().empty())
    {
        title += " - filtered";
    }

    this->titleLabel_->setText(title.isEmpty() ? "<empty>" : title);
}

void SplitHeader::updateIcons()
{
    auto channel = this->split_->getSelectedChannel();

    if (channel->isTwitchOrKickChannel())
    {
        if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get());
            twitchChannel != nullptr && !twitchChannel->isEmpty())
        {
            if (!getSettings()->showFollowButtonInSplitHeader ||
                !canUseFollowButtonForChannel(*twitchChannel))
            {
                this->followButton_->hide();
            }
            else
            {
                const auto following =
                    twitchChannel->isFollowingStatusKnown() &&
                    twitchChannel->isFollowing();
                const auto displayName = channel->getLocalizedName().isEmpty()
                                             ? channel->getName()
                                             : channel->getLocalizedName();
                this->followButton_->setSource(followButtonSource(following));
                this->followButton_->setToolTip(
                    following ? QString("Unfollow %1").arg(displayName)
                              : QString("Follow %1").arg(displayName));
                this->followButton_->show();
            }
        }
        else
        {
            this->followButton_->hide();
        }

        auto moderationMode = this->split_->getModerationMode() &&
                              !getSettings()->moderationActions.empty();

        if (moderationMode)
        {
            this->moderationButton_->setSource({
                .dark = ":/buttons/moderationEnabled-darkMode.svg",
                .light = ":/buttons/moderationEnabled-lightMode.svg",
            });
        }
        else
        {
            this->moderationButton_->setSource({
                .dark = ":/buttons/moderationDisabled-darkMode.svg",
                .light = ":/buttons/moderationDisabled-lightMode.svg",
            });
        }

        if (channel->hasModRights() || moderationMode)
        {
            this->moderationButton_->show();
        }
        else
        {
            this->moderationButton_->hide();
        }

        if (channel->hasModRights() && channel->isTwitchChannel())
        {
            this->chattersButton_->show();
        }
        else
        {
            this->chattersButton_->hide();
        }
    }
    else
    {
        this->followButton_->hide();
        this->moderationButton_->hide();
        this->chattersButton_->hide();
    }
}

void SplitHeader::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);

    QColor background = this->theme->splits.header.background;
    QColor border = this->theme->splits.header.border;

    if (this->split_->hasFocus())
    {
        background = this->theme->splits.header.focusedBackground;
        border = this->theme->splits.header.focusedBorder;
    }

    painter.fillRect(this->rect(), background);
    painter.setPen(border);
    painter.drawRect(0, 0, this->width() - 1, this->height() - 2);
    painter.fillRect(0, this->height() - 1, this->width(), 1, background);
}

void SplitHeader::mousePressEvent(QMouseEvent *event)
{
    switch (event->button())
    {
        case Qt::LeftButton: {
            this->split_->setFocus(Qt::MouseFocusReason);

            this->dragging_ = true;

            this->dragStart_ = event->pos();
        }
        break;

        case Qt::RightButton: {
            auto *menu = this->createMainMenu().release();
            menu->setAttribute(Qt::WA_DeleteOnClose);
            menu->popup(this->mapToGlobal(event->pos() + QPoint(0, 4)));
        }
        break;

        case Qt::MiddleButton: {
            this->split_->openInBrowser();
        }
        break;

        default: {
        }
        break;
    }

    this->doubleClicked_ = false;
}

void SplitHeader::mouseReleaseEvent(QMouseEvent * /*event*/)
{
    this->dragging_ = false;
}

void SplitHeader::mouseMoveEvent(QMouseEvent *event)
{
    if (this->dragging_)
    {
        if (distance(this->dragStart_, event->pos()) > 15 * this->scale())
        {
            this->split_->drag();
            this->dragging_ = false;
        }
    }
}

void SplitHeader::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        this->split_->changeChannel();
    }
    this->doubleClicked_ = true;
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
void SplitHeader::enterEvent(QEnterEvent *event)
#else
void SplitHeader::enterEvent(QEvent *event)
#endif
{
    if (!this->tooltipText_.isEmpty())
    {
        this->tooltipWidget_->setOne({nullptr, this->tooltipText_});
        this->tooltipWidget_->setWordWrap(true);
        this->tooltipWidget_->adjustSize();

        // On Windows, a lot of the resizing/activating happens when calling
        // show() and calling it doesn't synchronously create a visible window,
        // so moving the window won't cause the visible window to jump.
        //
        // On other platforms, this isn't the case, hence we call show() after
        // moving.
#ifdef Q_OS_WIN
        this->tooltipWidget_->show();
#endif

        auto pos =
            this->mapToGlobal(this->rect().bottomLeft()) +
            QPoint((this->width() - this->tooltipWidget_->width()) / 2, 1);

        this->tooltipWidget_->moveTo(pos,
                                     widgets::BoundsChecking::CursorPosition);

#ifndef Q_OS_WIN
        this->tooltipWidget_->show();
#endif
    }

    BaseWidget::enterEvent(event);
}

void SplitHeader::leaveEvent(QEvent *event)
{
    this->tooltipWidget_->hide();

    BaseWidget::leaveEvent(event);
}

void SplitHeader::themeChangedEvent()
{
    auto palette = QPalette();

    if (this->split_->hasFocus())
    {
        palette.setColor(QPalette::WindowText,
                         this->theme->splits.header.focusedText);
    }
    else
    {
        palette.setColor(QPalette::WindowText, this->theme->splits.header.text);
    }
    this->titleLabel_->setPalette(palette);

    auto bg = this->theme->splits.header.background;
    this->addButton_->setOptions({
        .background = bg,
        .backgroundHover = bg,
    });

    this->update();
}

void SplitHeader::reloadChannelEmotes()
{
    using namespace std::chrono_literals;

    auto now = std::chrono::steady_clock::now();
    if (this->lastReloadedChannelEmotes_ + 1s > now)
    {
        return;
    }
    this->lastReloadedChannelEmotes_ = now;

    auto channel = this->split_->getSelectedChannel();

    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        twitchChannel->refreshFFZChannelEmotes(true);
        twitchChannel->refreshBTTVChannelEmotes(true);
        twitchChannel->refreshSevenTVChannelEmotes(true);
        twitchChannel->refreshBadgesProviders();
    }
    else if (auto *kc = dynamic_cast<KickChannel *>(channel.get()))
    {
        kc->reloadSeventvEmotes(true);
    }
}

void SplitHeader::reloadSubscriberEmotes()
{
    using namespace std::chrono_literals;

    auto now = std::chrono::steady_clock::now();
    if (this->lastReloadedSubEmotes_ + 1s > now)
    {
        return;
    }
    this->lastReloadedSubEmotes_ = now;

    auto channel = this->split_->getSelectedChannel();
    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        twitchChannel->refreshTwitchChannelEmotes(true);
    }
}

void SplitHeader::reconnect()
{
    this->split_->getChannel()->reconnect();
}

}  // namespace chatterino

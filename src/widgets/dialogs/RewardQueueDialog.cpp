#include "widgets/dialogs/RewardQueueDialog.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/PubSubManager.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"
#include "util/PostToThread.hpp"

#include <QCheckBox>
#include <QCursor>
#include <QDateTime>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <utility>

namespace {

using namespace chatterino;

constexpr QSize DEFAULT_DIALOG_SIZE(760, 520);
constexpr int SIDEBAR_WIDTH = 210;
constexpr int BADGE_SIZE = 18;
constexpr int REWARD_ICON_SIZE = 20;
constexpr int STATUS_UPDATE_BATCH_SIZE = 50;
constexpr int MAX_UPDATE_ALL_ROUNDS = 40;
constexpr int RELOAD_DELAY_MS = 750;
constexpr int TICK_INTERVAL_MS = 30 * 1000;

void clearLayout(QLayout *layout)
{
    while (auto *item = layout->takeAt(0))
    {
        if (auto *widget = item->widget())
        {
            widget->deleteLater();
        }
        delete item;
    }
}

QString formatAge(const QDateTime &timestamp)
{
    if (!timestamp.isValid())
    {
        return {};
    }

    const auto seconds =
        std::max<qint64>(0, timestamp.secsTo(QDateTime::currentDateTimeUtc()));
    if (seconds < 60)
    {
        return QStringLiteral("just now");
    }
    if (seconds < 60 * 60)
    {
        return QStringLiteral("%1m ago").arg(seconds / 60);
    }
    if (seconds < 24 * 60 * 60)
    {
        return QStringLiteral("%1h ago").arg(seconds / (60 * 60));
    }
    return QStringLiteral("%1d ago").arg(seconds / (24 * 60 * 60));
}

QString userDisplayText(const GqlRewardQueueUser &user)
{
    if (user.displayName.isEmpty())
    {
        return user.login;
    }
    if (user.login.isEmpty() ||
        user.displayName.compare(user.login, Qt::CaseInsensitive) == 0)
    {
        return user.displayName;
    }
    return QStringLiteral("%1 (%2)").arg(user.displayName, user.login);
}

QPixmap pausePixmap(int size, const QColor &color)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    const auto barWidth = size * 0.22;
    const auto barHeight = size * 0.64;
    const auto top = (size - barHeight) / 2.0;
    painter.drawRoundedRect(QRectF(size * 0.2, top, barWidth, barHeight), 1, 1);
    painter.drawRoundedRect(QRectF(size * 0.58, top, barWidth, barHeight), 1,
                            1);
    return pixmap;
}

QString badgeImageUrl(const GqlBadge &badge)
{
    if (!badge.image2x.isEmpty())
    {
        return badge.image2x;
    }
    if (!badge.image4x.isEmpty())
    {
        return badge.image4x;
    }
    return badge.image1x;
}

QString channelIdFromPayload(const QJsonObject &data)
{
    auto channelId = data.value("channel_id").toString();
    if (channelId.isEmpty())
    {
        channelId =
            data.value("reward").toObject().value("channel_id").toString();
    }
    return channelId;
}

}  // namespace

namespace chatterino {

std::vector<QPointer<RewardQueueDialog>> RewardQueueDialog::activeDialogs_;

RewardQueueDialog::RewardQueueDialog(TwitchChannel *channel, QWidget *parent)
    : DraggablePopup(false, parent)
    , channelLogin_(channel->getName())
    , channelId_(channel->roomId())
{
    this->setObjectName("RewardQueueDialog");
    this->setWindowTitle(
        QStringLiteral("%1's Reward Queue").arg(this->channelLogin_));
    this->setScaleIndependentSize(DEFAULT_DIALOG_SIZE);

    auto *container = this->getLayoutContainer();
    container->setObjectName("RewardQueueDialogRoot");
    auto *mainLayout = new QHBoxLayout(container);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(8);

    this->sidebarScrollArea_ = new QScrollArea(container);
    this->sidebarScrollArea_->setObjectName("RewardQueueScrollArea");
    this->sidebarScrollArea_->setFrameShape(QFrame::NoFrame);
    this->sidebarScrollArea_->setWidgetResizable(true);
    this->sidebarScrollArea_->setHorizontalScrollBarPolicy(
        Qt::ScrollBarAlwaysOff);
    auto *sidebarWidget = new QWidget();
    sidebarWidget->setObjectName("RewardQueueContent");
    this->sidebarLayout_ = new QVBoxLayout(sidebarWidget);
    this->sidebarLayout_->setContentsMargins(0, 0, 0, 0);
    this->sidebarLayout_->setSpacing(2);
    this->sidebarScrollArea_->setWidget(sidebarWidget);
    mainLayout->addWidget(this->sidebarScrollArea_);

    auto *contentLayout = new QVBoxLayout();
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(6);
    mainLayout->addLayout(contentLayout, 1);

    this->titleLabel_ = new QLabel(container);
    this->titleLabel_->setObjectName("RewardQueueTitle");
    this->titleLabel_->setTextFormat(Qt::PlainText);
    this->titleLabel_->setWordWrap(true);
    auto *titleLayout = new QHBoxLayout();
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(4);
    contentLayout->addLayout(titleLayout);
    titleLayout->addWidget(this->titleLabel_, 1);

    this->pauseCheckBox_ = new QCheckBox("Pause redemptions", container);
    this->pauseCheckBox_->setToolTip(
        "Stops viewers from redeeming this reward until it is unpaused");
    this->pauseCheckBox_->hide();
    QObject::connect(this->pauseCheckBox_, &QCheckBox::clicked, this,
                     [this](bool checked) {
                         this->togglePaused(checked);
                     });
    titleLayout->addWidget(this->pauseCheckBox_);

    auto *actionsLayout = new QHBoxLayout();
    actionsLayout->setContentsMargins(0, 0, 0, 0);
    actionsLayout->setSpacing(4);
    contentLayout->addLayout(actionsLayout);

    this->selectAllCheckBox_ = new QCheckBox("Select all", container);
    QObject::connect(this->selectAllCheckBox_, &QCheckBox::clicked, this,
                     [this](bool checked) {
                         for (auto *checkBox : this->rowCheckBoxes_)
                         {
                             checkBox->setChecked(checked);
                         }
                     });
    actionsLayout->addWidget(this->selectAllCheckBox_);

    this->sortButton_ = new QPushButton(container);
    this->sortButton_->setObjectName("RewardQueueButton");
    this->updateSortButton();
    QObject::connect(this->sortButton_, &QPushButton::clicked, this, [this] {
        this->toggleSortOrder();
    });
    actionsLayout->addWidget(this->sortButton_);
    actionsLayout->addStretch(1);

    this->completeSelectedButton_ = new QPushButton(container);
    this->completeSelectedButton_->setObjectName("RewardQueueCompleteButton");
    QObject::connect(this->completeSelectedButton_, &QPushButton::clicked, this,
                     [this] {
                         this->updateRedemptions(this->selectedIds(), true);
                     });
    actionsLayout->addWidget(this->completeSelectedButton_);

    this->rejectSelectedButton_ = new QPushButton(container);
    this->rejectSelectedButton_->setObjectName("RewardQueueButton");
    QObject::connect(this->rejectSelectedButton_, &QPushButton::clicked, this,
                     [this] {
                         this->updateRedemptions(this->selectedIds(), false);
                     });
    actionsLayout->addWidget(this->rejectSelectedButton_);

    this->completeAllButton_ = new QPushButton("Complete all", container);
    this->completeAllButton_->setObjectName("RewardQueueCompleteButton");
    QObject::connect(this->completeAllButton_, &QPushButton::clicked, this,
                     [this] {
                         this->updateAll(true);
                     });
    actionsLayout->addWidget(this->completeAllButton_);

    this->rejectAllButton_ = new QPushButton("Reject all", container);
    this->rejectAllButton_->setObjectName("RewardQueueButton");
    QObject::connect(this->rejectAllButton_, &QPushButton::clicked, this,
                     [this] {
                         this->updateAll(false);
                     });
    actionsLayout->addWidget(this->rejectAllButton_);

    this->listScrollArea_ = new QScrollArea(container);
    this->listScrollArea_->setObjectName("RewardQueueScrollArea");
    this->listScrollArea_->setFrameShape(QFrame::NoFrame);
    this->listScrollArea_->setWidgetResizable(true);
    this->listScrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *listWidget = new QWidget();
    listWidget->setObjectName("RewardQueueContent");
    this->listLayout_ = new QVBoxLayout(listWidget);
    this->listLayout_->setContentsMargins(0, 0, 0, 0);
    this->listLayout_->setSpacing(4);
    this->listScrollArea_->setWidget(listWidget);
    contentLayout->addWidget(this->listScrollArea_, 1);

    auto *footerLayout = new QHBoxLayout();
    footerLayout->setContentsMargins(0, 0, 0, 0);
    footerLayout->setSpacing(4);
    contentLayout->addLayout(footerLayout);

    this->statusLabel_ = new QLabel(container);
    this->statusLabel_->setObjectName("RewardQueueStatus");
    this->statusLabel_->setTextFormat(Qt::PlainText);
    this->statusLabel_->setWordWrap(true);
    footerLayout->addWidget(this->statusLabel_, 1);

    this->loadMoreButton_ = new QPushButton("Load more", container);
    this->loadMoreButton_->setObjectName("RewardQueueButton");
    QObject::connect(this->loadMoreButton_, &QPushButton::clicked, this,
                     [this] {
                         this->loadRedemptions(true);
                     });
    footerLayout->addWidget(this->loadMoreButton_);

    for (auto *button : {
             this->completeSelectedButton_,
             this->rejectSelectedButton_,
             this->completeAllButton_,
             this->rejectAllButton_,
             this->loadMoreButton_,
             this->sortButton_,
         })
    {
        button->setCursor(Qt::PointingHandCursor);
    }

    this->reloadTimer_.setSingleShot(true);
    this->reloadTimer_.setInterval(RELOAD_DELAY_MS);
    QObject::connect(&this->reloadTimer_, &QTimer::timeout, this, [this] {
        if (this->loading_ || this->actionInFlight_)
        {
            this->reloadTimer_.start();
            return;
        }
        this->reload(true);
    });

    this->tickTimer_.setInterval(TICK_INTERVAL_MS);
    QObject::connect(&this->tickTimer_, &QTimer::timeout, this, [this] {
        this->onTick();
    });
    this->tickTimer_.start();

    QPointer<RewardQueueDialog> self = this;
    auto *pubSub = getApp()->getTwitchPubSub();
    auto connect = [this, self](auto &signal, auto handler) {
        this->managedConnections_.emplace_back(
            signal.connect([self, handler](const QJsonObject &data) {
                postToThread([self, handler, data] {
                    if (!self || channelIdFromPayload(data) != self->channelId_)
                    {
                        return;
                    }
                    handler(self.data(), data);
                });
            }));
    };
    connect(pubSub->pointReward.redeemed,
            [](RewardQueueDialog *dialog, const QJsonObject &data) {
                dialog->onRewardRedeemed(data);
            });
    connect(pubSub->pointReward.statusUpdated,
            [](RewardQueueDialog *dialog, const QJsonObject &data) {
                dialog->onRedemptionStatusUpdated(data);
            });
    connect(pubSub->pointReward.bulkUpdateProgressed,
            [](RewardQueueDialog *dialog, const QJsonObject &data) {
                dialog->onBulkUpdateProgress(data, false);
            });
    connect(pubSub->pointReward.bulkUpdateFinished,
            [](RewardQueueDialog *dialog, const QJsonObject &data) {
                dialog->onBulkUpdateProgress(data, true);
            });

    this->refreshStyle();
    this->rebuildSidebar();
    this->rebuildList();
    this->applySizeConstraints();
}

void RewardQueueDialog::showDialog(TwitchChannel *channel, QWidget *parent)
{
    if (channel == nullptr)
    {
        return;
    }

    const bool wasAutoPinned = DraggablePopup::pinParentIfNeeded(parent);

    RewardQueueDialog *dialog = nullptr;
    for (auto it = activeDialogs_.begin(); it != activeDialogs_.end();)
    {
        if (it->isNull())
        {
            it = activeDialogs_.erase(it);
            continue;
        }
        if ((*it)->channelLogin_ == channel->getName())
        {
            dialog = it->data();
            dialog->raise();
            dialog->activateWindow();
            dialog->reload();
            break;
        }
        ++it;
    }

    if (dialog == nullptr)
    {
        // Keep using `parent` for placement, but do not make another
        // DraggablePopup the QObject owner. Otherwise closing that popup
        // (for example channel rewards) destroys the queue with it.
        QWidget *ownershipParent = parent;
        if (qobject_cast<DraggablePopup *>(parent) != nullptr)
        {
            ownershipParent = nullptr;
        }

        dialog = new RewardQueueDialog(channel, ownershipParent);
        activeDialogs_.push_back(dialog);

        QPoint center = QCursor::pos();
        if (parent != nullptr && parent->window() != nullptr)
        {
            center = parent->window()->geometry().center();
        }

        dialog->show();
        const auto size = dialog->size();
        dialog->showAndMoveTo(
            center - QPoint(size.width() / 2, size.height() / 2),
            widgets::BoundsChecking::DesiredPosition);
        dialog->raise();
        dialog->activateWindow();
        dialog->reload();
    }

    if (wasAutoPinned)
    {
        dialog->scheduleUnpinParentOnClose(parent);
    }
}

void RewardQueueDialog::scheduleUnpinParentOnClose(QWidget *parent)
{
    if (this->parentUnpinScheduled_ || parent == nullptr)
    {
        return;
    }

    this->parentUnpinScheduled_ = true;

    QPointer<QWidget> parentPtr(parent);
    QObject::connect(this, &QObject::destroyed, parent, [parentPtr] {
        if (parentPtr)
        {
            DraggablePopup::unpinParentIfNeeded(parentPtr);
        }
    });
}

void RewardQueueDialog::themeChangedEvent()
{
    DraggablePopup::themeChangedEvent();
    this->refreshStyle();
    this->rebuildSidebar(true);
    this->rebuildList();
}

void RewardQueueDialog::scaleChangedEvent(float scale)
{
    DraggablePopup::scaleChangedEvent(scale);
    this->refreshStyle();
    this->rebuildSidebar(true);
    this->rebuildList();
    // setScale locks the size again after this returns.
    QTimer::singleShot(0, this, [this] {
        this->applySizeConstraints();
    });
}

void RewardQueueDialog::showEvent(QShowEvent *event)
{
    DraggablePopup::showEvent(event);
    this->applySizeConstraints();
}

void RewardQueueDialog::applySizeConstraints()
{
    const int minWidth =
        std::max(420, int(DEFAULT_DIALOG_SIZE.width() * 0.65F * this->scale()));
    const int minHeight = std::max(
        280, int(DEFAULT_DIALOG_SIZE.height() * 0.65F * this->scale()));
    const int defaultWidth =
        std::max(minWidth, int(DEFAULT_DIALOG_SIZE.width() * this->scale()));
    const int defaultHeight =
        std::max(minHeight, int(DEFAULT_DIALOG_SIZE.height() * this->scale()));

    int maxWidth = QWIDGETSIZE_MAX;
    int maxHeight = QWIDGETSIZE_MAX;
    auto *screen = this->screen();
    if (screen == nullptr)
    {
        screen = QGuiApplication::screenAt(QCursor::pos());
    }
    if (screen == nullptr)
    {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen != nullptr)
    {
        const auto available = screen->availableGeometry();
        const int margin = std::max(12, int(16 * this->scale()));
        maxWidth = std::max(minWidth, available.width() - margin * 2);
        maxHeight = std::max(minHeight, available.height() - margin * 2);
    }

    this->setMinimumSize(minWidth, minHeight);
    this->setMaximumSize(maxWidth, maxHeight);
    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    if (this->width() <= 0 || this->height() <= 0)
    {
        this->resize(defaultWidth, defaultHeight);
        return;
    }

    this->resize(qBound(minWidth, this->width(), maxWidth),
                 qBound(minHeight, this->height(), maxHeight));
}

void RewardQueueDialog::refreshStyle()
{
    auto *fonts = getApp()->getFonts();
    const auto scale = this->scale();
    this->getLayoutContainer()->setFont(
        fonts->getFont(FontStyle::UiMedium, scale));
    this->titleLabel_->setFont(fonts->getFont(FontStyle::UiMediumBold, scale));
    this->sidebarScrollArea_->setFixedWidth(int(SIDEBAR_WIDTH * scale));

    const auto *theme = this->theme;
    auto mutedColor = theme->window.text;
    mutedColor.setAlpha(160);
    const auto bg = theme->window.background.name();
    const auto text = theme->window.text.name(QColor::HexArgb);
    const auto border = theme->splits.header.border.name();
    const auto muted = mutedColor.name(QColor::HexArgb);
    const auto buttonBg = theme->splits.input.background.name();
    const auto hoverBg =
        theme->isLightTheme()
            ? theme->splits.input.background.darker(104).name()
            : theme->splits.input.background.lighter(108).name();
    const auto focusedBorder = theme->splits.header.focusedBorder.name();
    const int radius = std::max(1, int(2 * scale));
    const int padding = std::max(2, int(4 * scale));

    this->setStyleSheet(
        QStringLiteral(R"(
        QWidget#RewardQueueDialogRoot {
            background: %1;
            color: %2;
        }
        QWidget#RewardQueueContent {
            background: transparent;
        }
        QScrollArea#RewardQueueScrollArea {
            background: transparent;
            border: 0;
        }
        QLabel, QCheckBox {
            color: %2;
        }
        QLabel#RewardQueueMuted, QLabel#RewardQueueStatus {
            color: %4;
        }
        QFrame#RewardQueueRow {
            background: %5;
            border: 1px solid %3;
            border-radius: %8px;
        }
        QPushButton#RewardQueueButton,
        QPushButton#RewardQueueCompleteButton,
        QPushButton#RewardQueueSidebarButton {
            background: %5;
            color: %2;
            border: 1px solid %3;
            border-radius: %8px;
            padding: %9px %10px;
        }
        QPushButton#RewardQueueSidebarButton {
            text-align: left;
            padding: 0;
        }
        QLabel#RewardQueueSidebarLabel, QLabel#RewardQueueSidebarIcon {
            background: transparent;
        }
        QPushButton#RewardQueueSidebarButton[rewardDisabled="true"]
            QLabel#RewardQueueSidebarLabel {
            color: %4;
        }
        QPushButton#RewardQueueSidebarButton:checked
            QLabel#RewardQueueSidebarLabel {
            font-weight: 700;
        }
        QPushButton#RewardQueueCompleteButton {
            font-weight: 700;
        }
        QPushButton#RewardQueueButton:hover,
        QPushButton#RewardQueueCompleteButton:hover,
        QPushButton#RewardQueueSidebarButton:hover {
            background: %6;
            border-color: %7;
        }
        QPushButton#RewardQueueSidebarButton[rewardDisabled="true"] {
            color: %4;
        }
        QPushButton#RewardQueueSidebarButton:checked {
            border-color: %7;
        }
        QPushButton#RewardQueueButton:disabled,
        QPushButton#RewardQueueCompleteButton:disabled {
            color: %4;
            border-color: %3;
        }
    )")
            .arg(bg, text, border, muted, buttonBg, hoverBg, focusedBorder)
            .arg(radius)
            .arg(padding)
            .arg(padding * 2));
}

void RewardQueueDialog::reload(bool quiet)
{
    const auto token = this->authTokenOrMessage("viewing the reward queue");
    if (token.isEmpty())
    {
        this->updateAllStatus_.reset();
        return;
    }

    this->reloadTimer_.stop();
    this->loading_ = true;
    this->quietLoad_ = quiet;
    const auto generation = ++this->generation_;
    if (!quiet)
    {
        this->setStatus("Loading reward queue...");
        this->updateActions();
    }

    QPointer<RewardQueueDialog> self = this;
    TwitchGql::getRewardQueue(
        this->channelLogin_, token,
        [self, generation](GqlRewardQueue queue) {
            if (!self || generation != self->generation_)
            {
                return;
            }
            self->applyRewardQueue(std::move(queue));
            self->loadRedemptions(false);
        },
        [self, generation](const QString &error) {
            if (!self || generation != self->generation_)
            {
                return;
            }
            self->loading_ = false;
            self->updateAllStatus_.reset();
            self->setStatus(MoltorinoAuth::normalizeAuthError(
                                "loading the reward queue", error),
                            true);
            self->updateActions();
        });
}

void RewardQueueDialog::refreshCounts()
{
    if (this->countsRefreshInFlight_)
    {
        return;
    }

    const auto token = this->authTokenOrMessage("viewing the reward queue");
    if (token.isEmpty())
    {
        return;
    }

    this->countsRefreshInFlight_ = true;
    const auto generation = this->generation_;
    QPointer<RewardQueueDialog> self = this;
    TwitchGql::getRewardQueue(
        this->channelLogin_, token,
        [self, generation](GqlRewardQueue queue) {
            if (!self || generation != self->generation_)
            {
                if (self)
                {
                    self->countsRefreshInFlight_ = false;
                }
                return;
            }
            self->countsRefreshInFlight_ = false;
            self->applyRewardQueue(std::move(queue));
        },
        [self, generation](const QString &error) {
            if (!self || generation != self->generation_)
            {
                if (self)
                {
                    self->countsRefreshInFlight_ = false;
                }
                return;
            }
            self->countsRefreshInFlight_ = false;
            self->setStatus(MoltorinoAuth::normalizeAuthError(
                                "loading the reward queue", error),
                            true);
        });
}

void RewardQueueDialog::applyRewardQueue(GqlRewardQueue queue)
{
    if (!queue.channelId.isEmpty() && queue.channelId != this->channelId_)
    {
        this->channelId_ = queue.channelId;
        getApp()->getTwitchPubSub()->listenToChannelPointRewards(
            this->channelId_);
    }

    for (const auto &known : this->rewards_)
    {
        const auto stillListed =
            std::ranges::any_of(queue.rewards, [&known](const auto &reward) {
                return reward.id == known.id;
            });
        if (!stillListed && known.id == this->selectedRewardId_)
        {
            auto kept = known;
            kept.count = 0;
            queue.rewards.push_back(kept);
        }
    }
    for (const auto &reward : queue.rewards)
    {
        if (!this->rewardOrder_.contains(reward.id))
        {
            this->rewardOrder_.push_back(reward.id);
        }
    }
    std::ranges::stable_sort(queue.rewards, {}, [this](const auto &reward) {
        return this->rewardOrder_.indexOf(reward.id);
    });
    this->rewards_ = std::move(queue.rewards);
    const auto selectedExists =
        std::ranges::any_of(this->rewards_, [this](const auto &reward) {
            return reward.id == this->selectedRewardId_;
        });
    if (!selectedExists)
    {
        this->selectedRewardId_.clear();
    }
    this->countsValidAt_ = QDateTime::currentDateTimeUtc();
    this->rebuildSidebar();
}

void RewardQueueDialog::loadRedemptions(bool append)
{
    const auto token = this->authTokenOrMessage("viewing the reward queue");
    if (token.isEmpty())
    {
        this->loading_ = false;
        this->updateAllStatus_.reset();
        this->updateActions();
        return;
    }

    this->loading_ = true;
    if (append)
    {
        this->quietLoad_ = false;
    }
    if (!this->quietLoad_)
    {
        this->updateActions();
    }

    const auto generation = this->generation_;
    QPointer<RewardQueueDialog> self = this;
    auto failureCallback = [self, generation](const QString &error) {
        if (!self || generation != self->generation_)
        {
            return;
        }
        self->loading_ = false;
        self->updateAllStatus_.reset();
        self->setStatus(
            MoltorinoAuth::normalizeAuthError("loading reward requests", error),
            true);
        self->updateActions();
    };

    if (append && this->selectedRewardId_.isEmpty())
    {
        QStringList rewardIds;
        for (const auto &reward : this->rewards_)
        {
            if (reward.count > 0 && !reward.id.isEmpty())
            {
                rewardIds.push_back(reward.id);
            }
        }
        if (!rewardIds.isEmpty())
        {
            struct PageBatch {
                int remaining = 0;
                int added = 0;
                bool anyNextPage = false;
                QString error;
                QStringList missingUsers;
            };
            auto batch = std::make_shared<PageBatch>();
            batch->remaining = static_cast<int>(rewardIds.size());

            auto finishBatch = [self, generation, batch] {
                if (!self || generation != self->generation_)
                {
                    return;
                }

                const auto newest = self->newestFirst_;
                std::stable_sort(
                    self->redemptions_.begin(), self->redemptions_.end(),
                    [newest](const GqlRewardRedemption &left,
                             const GqlRewardRedemption &right) {
                        if (newest)
                        {
                            return left.timestamp > right.timestamp;
                        }
                        return left.timestamp < right.timestamp;
                    });
                self->extraPagesLoaded_ = true;
                self->nextCursor_.clear();
                self->hasNextPage_ = batch->anyNextPage;
                if (batch->added == 0 && !batch->error.isEmpty())
                {
                    self->loading_ = false;
                    self->updateAllStatus_.reset();
                    self->setStatus(
                        MoltorinoAuth::normalizeAuthError(
                            "loading reward requests", batch->error),
                        true);
                    self->updateActions();
                    return;
                }
                self->loadUsers(batch->missingUsers, [self, generation] {
                    if (!self || generation != self->generation_)
                    {
                        return;
                    }
                    self->finishLoading();
                });
            };

            for (const auto &rewardId : rewardIds)
            {
                TwitchGql::getRewardQueueRedemptions(
                    this->channelLogin_, rewardId, QString(),
                    this->newestFirst_, token,
                    [self, generation, batch,
                     finishBatch](GqlRewardRedemptionPage page) {
                        if (self && generation == self->generation_)
                        {
                            batch->anyNextPage =
                                batch->anyNextPage || page.hasNextPage;
                            for (const auto &redemption : page.redemptions)
                            {
                                if (self->resolvedIds_.contains(
                                        redemption.id) ||
                                    self->findRedemption(redemption.id) !=
                                        nullptr)
                                {
                                    continue;
                                }
                                self->redemptions_.push_back(redemption);
                                ++batch->added;
                                const auto needsUser =
                                    !redemption.userId.isEmpty() &&
                                    (!self->users_.contains(
                                         redemption.userId) ||
                                     self->badgeFetchFailed_.contains(
                                         redemption.userId));
                                if (needsUser && !batch->missingUsers.contains(
                                                     redemption.userId))
                                {
                                    batch->missingUsers.push_back(
                                        redemption.userId);
                                }
                            }
                        }
                        if (--batch->remaining == 0)
                        {
                            finishBatch();
                        }
                    },
                    [batch, finishBatch](const QString &error) {
                        if (batch->error.isEmpty())
                        {
                            batch->error = error;
                        }
                        if (--batch->remaining == 0)
                        {
                            finishBatch();
                        }
                    });
            }
            return;
        }
    }

    TwitchGql::getRewardQueueRedemptions(
        this->channelLogin_, this->selectedRewardId_,
        append ? this->nextCursor_ : QString(), this->newestFirst_, token,
        [self, generation, append](GqlRewardRedemptionPage page) {
            if (!self || generation != self->generation_)
            {
                return;
            }

            if (!append)
            {
                self->redemptions_.clear();
            }
            self->extraPagesLoaded_ = append;
            self->nextCursor_ = page.nextCursor;
            self->hasNextPage_ = page.hasNextPage && !page.nextCursor.isEmpty();

            QStringList missingUsers;
            for (const auto &redemption : page.redemptions)
            {
                if (self->resolvedIds_.contains(redemption.id) ||
                    self->findRedemption(redemption.id) != nullptr)
                {
                    continue;
                }
                self->redemptions_.push_back(redemption);
                const auto needsUser =
                    !redemption.userId.isEmpty() &&
                    (!self->users_.contains(redemption.userId) ||
                     self->badgeFetchFailed_.contains(redemption.userId));
                if (needsUser && !missingUsers.contains(redemption.userId))
                {
                    missingUsers.push_back(redemption.userId);
                }
            }

            self->loadUsers(missingUsers, [self, generation] {
                if (!self || generation != self->generation_)
                {
                    return;
                }
                self->finishLoading();
            });
        },
        failureCallback);
}

void RewardQueueDialog::loadUsers(const QStringList &userIds,
                                  std::function<void()> callback)
{
    const auto auth = MoltorinoAuth::resolveModerationToken(
        this->channelId_, this->channelLogin_);
    if (userIds.isEmpty() || !auth.hasToken())
    {
        callback();
        return;
    }

    QPointer<RewardQueueDialog> self = this;
    TwitchGql::getRewardQueueUsers(
        this->channelLogin_, userIds, auth.token,
        [self, callback, userIds](QVector<GqlRewardQueueUser> users) {
            if (!self)
            {
                return;
            }
            for (const auto &id : userIds)
            {
                self->badgeFetchFailed_.remove(id);
            }
            for (const auto &user : users)
            {
                self->users_.insert(user.id, user);
            }
            callback();
        },
        [self, callback, userIds](const QString &) {
            if (self)
            {
                for (const auto &id : userIds)
                {
                    self->badgeFetchFailed_.insert(id);
                }
            }
            callback();
        });
}

void RewardQueueDialog::finishLoading()
{
    this->loading_ = false;
    if (!this->pendingError_.isEmpty())
    {
        this->setStatus(this->pendingError_, true);
        this->pendingError_.clear();
    }
    else if (!this->quietLoad_ || !this->statusIsError_)
    {
        this->setIdleStatus();
    }
    this->quietLoad_ = false;
    this->rebuildList();

    if (!this->updateAllStatus_)
    {
        return;
    }

    const auto ids = this->loadedIds();
    if (ids.isEmpty())
    {
        this->updateAllStatus_.reset();
        return;
    }
    if (this->updateAllRounds_ >= MAX_UPDATE_ALL_ROUNDS)
    {
        this->updateAllStatus_.reset();
        this->setStatus(
            "Stopped before the rest of the queue was updated. Run it again "
            "to continue.");
        return;
    }

    this->updateAllRounds_++;
    this->updateRedemptions(ids, *this->updateAllStatus_);
}

void RewardQueueDialog::onRewardRedeemed(const QJsonObject &redemption)
{
    const auto id = redemption.value("id").toString();
    const auto reward = redemption.value("reward").toObject();
    const auto rewardId = reward.value("id").toString();
    if (id.isEmpty() || rewardId.isEmpty() ||
        redemption.value("status").toString() != "UNFULFILLED" ||
        reward.value("should_redemptions_skip_request_queue").toBool(false))
    {
        return;
    }
    if (this->resolvedIds_.contains(id) || this->redeemedIds_.contains(id))
    {
        return;
    }
    this->redeemedIds_.insert(id);

    if (this->findRedemption(id) != nullptr)
    {
        return;
    }

    const auto timestamp = QDateTime::fromString(
        redemption.value("redeemed_at").toString(), Qt::ISODateWithMs);
    if (!this->isNewerThanSnapshot(timestamp))
    {
        return;
    }

    const auto rewardIt =
        std::ranges::find_if(this->rewards_, [&rewardId](const auto &r) {
            return r.id == rewardId;
        });
    if (this->loading_ || rewardIt == this->rewards_.end())
    {
        this->queueReload();
        return;
    }

    rewardIt->count++;
    this->rebuildSidebar();

    const auto matchesFilter = this->selectedRewardId_.isEmpty() ||
                               this->selectedRewardId_ == rewardId;
    if (!matchesFilter || (!this->newestFirst_ && this->hasNextPage_))
    {
        return;
    }

    const auto user = redemption.value("user").toObject();
    GqlRewardRedemption item;
    item.id = id;
    item.rewardId = rewardId;
    item.rewardTitle = reward.value("title").toString();
    item.userId = user.value("id").toString();
    item.input = redemption.value("user_input").toString();
    item.timestamp = timestamp;
    if (!item.timestamp.isValid())
    {
        item.timestamp = QDateTime::currentDateTimeUtc();
    }
    if (this->newestFirst_)
    {
        this->redemptions_.prepend(item);
    }
    else
    {
        this->redemptions_.push_back(item);
    }

    const auto needsUser = !item.userId.isEmpty() &&
                           (!this->users_.contains(item.userId) ||
                            this->badgeFetchFailed_.contains(item.userId));
    if (needsUser)
    {
        if (!this->users_.contains(item.userId))
        {
            GqlRewardQueueUser partialUser;
            partialUser.id = item.userId;
            partialUser.login = user.value("login").toString();
            partialUser.displayName = user.value("display_name").toString();
            this->users_.insert(partialUser.id, partialUser);
        }

        QPointer<RewardQueueDialog> self = this;
        this->loadUsers({item.userId}, [self] {
            if (self)
            {
                self->rebuildList();
            }
        });
    }

    if (!this->actionInFlight_ && !this->statusIsError_)
    {
        this->setIdleStatus();
    }
    this->rebuildList();
}

void RewardQueueDialog::onRedemptionStatusUpdated(const QJsonObject &redemption)
{
    const auto id = redemption.value("id").toString();
    const auto status = redemption.value("status").toString();
    if (id.isEmpty() || status.isEmpty() || status == "UNFULFILLED")
    {
        return;
    }
    if (this->resolvedIds_.contains(id))
    {
        return;
    }
    this->resolvedIds_.insert(id);

    if (this->loading_)
    {
        this->queueReload();
        return;
    }

    const auto timestamp = QDateTime::fromString(
        redemption.value("redeemed_at").toString(), Qt::ISODateWithMs);
    const auto wasVisible = this->findRedemption(id) != nullptr;
    this->resolveLocally(
        id, redemption.value("reward").toObject().value("id").toString(),
        wasVisible || this->isNewerThanSnapshot(timestamp));
    this->rebuildSidebar();
    this->rebuildList();
    if (!this->actionInFlight_ && !this->statusIsError_)
    {
        this->setIdleStatus();
    }
    if (this->redemptions_.isEmpty() && this->hasNextPage_)
    {
        this->queueReload();
    }
}

void RewardQueueDialog::onBulkUpdateProgress(const QJsonObject &progress,
                                             bool finished)
{
    if (finished)
    {
        if (!this->actionInFlight_ && !this->statusIsError_)
        {
            this->setIdleStatus();
        }
        this->queueReload();
        return;
    }

    const auto total = progress.value("total").toInt(0);
    if (total <= 0 || this->actionInFlight_)
    {
        return;
    }
    this->setStatus(QStringLiteral("Updating requests: %1 of %2")
                        .arg(progress.value("processed").toInt(0))
                        .arg(total));
}

void RewardQueueDialog::resolveLocally(const QString &redemptionId,
                                       const QString &rewardId,
                                       bool adjustCount)
{
    auto resolvedRewardId = rewardId;
    const auto it = std::ranges::find_if(
        this->redemptions_, [&redemptionId](const auto &redemption) {
            return redemption.id == redemptionId;
        });
    if (it != this->redemptions_.end())
    {
        if (resolvedRewardId.isEmpty())
        {
            resolvedRewardId = it->rewardId;
        }
        this->redemptions_.erase(it);
    }
    this->selected_.remove(redemptionId);

    if (!adjustCount)
    {
        return;
    }

    for (auto &reward : this->rewards_)
    {
        if (reward.id == resolvedRewardId)
        {
            reward.count = std::max(0, reward.count - 1);
            return;
        }
    }
}

bool RewardQueueDialog::isNewerThanSnapshot(const QDateTime &timestamp) const
{
    if (!this->countsValidAt_.isValid() || !timestamp.isValid())
    {
        return true;
    }
    return timestamp > this->countsValidAt_;
}

const GqlRewardRedemption *RewardQueueDialog::findRedemption(
    const QString &redemptionId) const
{
    for (const auto &redemption : this->redemptions_)
    {
        if (redemption.id == redemptionId)
        {
            return &redemption;
        }
    }
    return nullptr;
}

void RewardQueueDialog::setIdleStatus()
{
    this->setStatus(this->redemptions_.isEmpty()
                        ? QStringLiteral("No pending requests.")
                        : QString());
}

void RewardQueueDialog::onTick()
{
    for (const auto &[label, timestamp] : this->timeLabels_)
    {
        if (label)
        {
            label->setText(formatAge(timestamp));
        }
    }

    if (this->loading_ || this->actionInFlight_ ||
        this->reloadTimer_.isActive() || !this->isVisible())
    {
        return;
    }
    if (this->extraPagesLoaded_)
    {
        this->refreshCounts();
        return;
    }
    this->reload(true);
}

void RewardQueueDialog::rebuildSidebar(bool force)
{
    QStringList ids;
    ids.reserve(this->rewards_.size());
    for (const auto &reward : this->rewards_)
    {
        ids.push_back(reward.id);
    }

    const auto iconSize = int(REWARD_ICON_SIZE * this->scale());
    const auto pad = std::max(2, int(4 * this->scale()));
    if (force || ids != this->sidebarIds_ || this->sidebarButtons_.isEmpty())
    {
        clearLayout(this->sidebarLayout_);
        this->sidebarButtons_.clear();
        this->sidebarIds_ = ids;

        auto addButton = [this, iconSize, pad](const QString &rewardId,
                                               bool withIcon) {
            auto *button = new QPushButton();
            button->setObjectName("RewardQueueSidebarButton");
            button->setCheckable(true);
            button->setCursor(Qt::PointingHandCursor);
            button->setMinimumWidth(0);
            button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
            auto *row = new QHBoxLayout(button);
            row->setContentsMargins(pad * 2, pad, pad * 2, pad);
            row->setSpacing(pad);

            if (withIcon)
            {
                auto *iconLabel = new QLabel(button);
                iconLabel->setObjectName("RewardQueueSidebarIcon");
                iconLabel->setFixedSize(iconSize, iconSize);
                iconLabel->setAlignment(Qt::AlignCenter);
                iconLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
                row->addWidget(iconLabel, 0, Qt::AlignVCenter);
            }

            auto *textLabel = new QLabel(button);
            textLabel->setObjectName("RewardQueueSidebarLabel");
            textLabel->setWordWrap(true);
            textLabel->setTextFormat(Qt::PlainText);
            textLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            textLabel->setMinimumWidth(0);
            textLabel->setSizePolicy(QSizePolicy::Ignored,
                                     QSizePolicy::Minimum);
            textLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
            row->addWidget(textLabel, 1);

            QObject::connect(button, &QPushButton::clicked, this,
                             [this, rewardId] {
                                 this->selectReward(rewardId);
                             });
            this->sidebarLayout_->addWidget(button);
            this->sidebarButtons_.insert(rewardId, button);
        };

        addButton({}, false);
        for (const auto &id : ids)
        {
            addButton(id, true);
        }
        this->sidebarLayout_->addStretch(1);
    }

    int sidebarWidth = this->sidebarScrollArea_->viewport()->width();
    if (sidebarWidth <= 0)
    {
        sidebarWidth =
            int(SIDEBAR_WIDTH * this->scale()) -
            this->sidebarScrollArea_->verticalScrollBar()->sizeHint().width();
    }

    auto fitButton = [sidebarWidth](QPushButton *button) {
        auto *label = button->findChild<QLabel *>(
            QStringLiteral("RewardQueueSidebarLabel"));
        auto *layout = button->layout();
        if (label == nullptr || layout == nullptr)
        {
            return;
        }

        const auto margins = layout->contentsMargins();
        int available = sidebarWidth - margins.left() - margins.right();
        if (auto *icon = button->findChild<QLabel *>(
                QStringLiteral("RewardQueueSidebarIcon")))
        {
            available -= icon->width() + layout->spacing();
        }
        available = std::max(available, 1);

        const int textHeight = label->heightForWidth(available);
        label->setMinimumHeight(textHeight);
        button->setMinimumHeight(textHeight + margins.top() + margins.bottom());
    };

    auto setButtonText = [&fitButton](QPushButton *button,
                                      const QString &text) {
        if (auto *label = button->findChild<QLabel *>(
                QStringLiteral("RewardQueueSidebarLabel")))
        {
            label->setText(text);
        }
        fitButton(button);
    };

    int total = 0;
    const GqlRewardQueueReward *selectedReward = nullptr;
    for (const auto &reward : this->rewards_)
    {
        total += reward.count;
        if (reward.id == this->selectedRewardId_)
        {
            selectedReward = &reward;
        }

        auto *button = this->sidebarButtons_.value(reward.id);
        if (button == nullptr)
        {
            continue;
        }

        auto title = reward.title;
        if (!reward.isEnabled)
        {
            title += QStringLiteral(" (disabled)");
        }
        button->setChecked(reward.id == this->selectedRewardId_);
        setButtonText(button, QStringLiteral("%1 (%2)").arg(
                                  title, QLocale().toString(reward.count)));

        auto tooltip = QStringLiteral("%1 - %2 points")
                           .arg(reward.title, QLocale().toString(reward.cost));
        if (reward.isPaused)
        {
            tooltip += QStringLiteral("\nRedemptions are paused");
        }
        if (!reward.isEnabled)
        {
            tooltip += QStringLiteral("\nDisabled by the broadcaster");
        }
        if (!reward.prompt.isEmpty())
        {
            tooltip += "\n" + reward.prompt;
        }
        button->setToolTip(tooltip);

        if (button->property("rewardDisabled").toBool() != !reward.isEnabled)
        {
            button->setProperty("rewardDisabled", !reward.isEnabled);
            button->style()->unpolish(button);
            button->style()->polish(button);
            if (auto *label = button->findChild<QLabel *>(
                    QStringLiteral("RewardQueueSidebarLabel")))
            {
                label->style()->unpolish(label);
                label->style()->polish(label);
            }
            setButtonText(button, QStringLiteral("%1 (%2)").arg(
                                      title, QLocale().toString(reward.count)));
        }

        auto *iconLabel = button->findChild<QLabel *>(
            QStringLiteral("RewardQueueSidebarIcon"));
        if (iconLabel == nullptr)
        {
            continue;
        }

        if (reward.isPaused)
        {
            iconLabel->setPixmap(
                pausePixmap(iconSize, this->theme->window.text));
            continue;
        }

        iconLabel->clear();
        QPointer<RewardQueueDialog> self = this;
        QPointer<QLabel> iconPtr = iconLabel;
        this->loadPixmap(
            reward.imageUrl,
            [self, iconPtr, iconSize, id = reward.id](const QPixmap &pixmap) {
                if (!self || !iconPtr)
                {
                    return;
                }
                const auto paused =
                    std::ranges::any_of(self->rewards_, [&id](const auto &r) {
                        return r.id == id && r.isPaused;
                    });
                if (!paused)
                {
                    iconPtr->setPixmap(pixmap.scaled(iconSize, iconSize,
                                                     Qt::KeepAspectRatio,
                                                     Qt::SmoothTransformation));
                }
            });
    }

    if (auto *allButton = this->sidebarButtons_.value(QString()))
    {
        setButtonText(
            allButton,
            QStringLiteral("All requests (%1)").arg(QLocale().toString(total)));
        allButton->setChecked(this->selectedRewardId_.isEmpty());
    }

    this->titleLabel_->setText(selectedReward == nullptr
                                   ? QStringLiteral("All requests")
                                   : selectedReward->title);
    this->pauseCheckBox_->setVisible(selectedReward != nullptr);
    this->pauseCheckBox_->setChecked(selectedReward != nullptr &&
                                     selectedReward->isPaused);
    this->pauseCheckBox_->setEnabled(!this->pauseInFlight_);
}

void RewardQueueDialog::setRewardPaused(const QString &rewardId, bool paused)
{
    for (auto &reward : this->rewards_)
    {
        if (reward.id == rewardId)
        {
            reward.isPaused = paused;
        }
    }
    this->rebuildSidebar();
}

void RewardQueueDialog::togglePaused(bool paused)
{
    const auto rewardId = this->selectedRewardId_;
    if (rewardId.isEmpty() || this->pauseInFlight_)
    {
        this->rebuildSidebar();
        return;
    }

    const auto token = this->authTokenOrMessage("managing the reward queue");
    if (token.isEmpty())
    {
        this->rebuildSidebar();
        return;
    }

    this->pauseInFlight_ = true;
    this->setRewardPaused(rewardId, paused);

    QPointer<RewardQueueDialog> self = this;
    TwitchGql::pauseRewardRedemptions(
        this->channelId_, rewardId, paused, token,
        [self] {
            if (!self)
            {
                return;
            }
            self->pauseInFlight_ = false;
            self->rebuildSidebar();
        },
        [self, rewardId, paused](const QString &error) {
            if (!self)
            {
                return;
            }
            self->pauseInFlight_ = false;
            self->setRewardPaused(rewardId, !paused);
            self->setStatus(MoltorinoAuth::normalizeAuthError(
                                "managing the reward queue", error),
                            true);
        });
}

void RewardQueueDialog::rebuildList()
{
    const auto scrollValue =
        this->listScrollArea_->verticalScrollBar()->value();

    clearLayout(this->listLayout_);
    this->rowCheckBoxes_.clear();
    this->timeLabels_.clear();

    QSet<QString> stillSelected;
    for (const auto &redemption : this->redemptions_)
    {
        if (this->selected_.contains(redemption.id))
        {
            stillSelected.insert(redemption.id);
        }
    }
    this->selected_ = stillSelected;

    for (const auto &redemption : this->redemptions_)
    {
        this->listLayout_->addWidget(this->createRow(redemption));
    }
    this->listLayout_->addStretch(1);

    this->updateActions();

    QPointer<RewardQueueDialog> self = this;
    QTimer::singleShot(0, this, [self, scrollValue] {
        if (self)
        {
            self->listScrollArea_->verticalScrollBar()->setValue(scrollValue);
        }
    });
}

QWidget *RewardQueueDialog::createRow(const GqlRewardRedemption &redemption)
{
    const auto scale = this->scale();
    const int margin = std::max(3, int(6 * scale));

    auto *row = new QFrame();
    row->setObjectName("RewardQueueRow");
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(margin, margin, margin, margin);
    rowLayout->setSpacing(margin);

    auto *checkBox = new QCheckBox(row);
    checkBox->setChecked(this->selected_.contains(redemption.id));
    QObject::connect(checkBox, &QCheckBox::toggled, this,
                     [this, id = redemption.id](bool checked) {
                         if (checked)
                         {
                             this->selected_.insert(id);
                         }
                         else
                         {
                             this->selected_.remove(id);
                         }
                         this->updateActions();
                     });
    this->rowCheckBoxes_.insert(redemption.id, checkBox);
    rowLayout->addWidget(checkBox, 0, Qt::AlignVCenter);

    auto *textLayout = new QVBoxLayout();
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(std::max(1, int(2 * scale)));
    rowLayout->addLayout(textLayout, 1);

    auto *userLayout = new QHBoxLayout();
    userLayout->setContentsMargins(0, 0, 0, 0);
    userLayout->setSpacing(std::max(2, int(3 * scale)));
    textLayout->addLayout(userLayout);

    const auto userIt = this->users_.constFind(redemption.userId);
    const auto badgeSize = int(BADGE_SIZE * scale);
    if (userIt != this->users_.constEnd())
    {
        for (const auto &badge : userIt->badges)
        {
            const auto url = badgeImageUrl(badge);
            if (url.isEmpty())
            {
                continue;
            }

            auto *badgeLabel = new QLabel(row);
            badgeLabel->setFixedSize(badgeSize, badgeSize);
            badgeLabel->setScaledContents(true);
            badgeLabel->setToolTip(badge.title);
            userLayout->addWidget(badgeLabel);

            QPointer<QLabel> badgePtr = badgeLabel;
            this->loadPixmap(url, [badgePtr](const QPixmap &pixmap) {
                if (badgePtr)
                {
                    badgePtr->setPixmap(pixmap);
                }
            });
        }
    }

    auto *nameLabel = new QLabel(row);
    nameLabel->setTextFormat(Qt::PlainText);
    nameLabel->setFont(
        getApp()->getFonts()->getFont(FontStyle::UiMediumBold, scale));
    if (userIt != this->users_.constEnd())
    {
        nameLabel->setText(userDisplayText(*userIt));
        const QColor color(userIt->color);
        if (!userIt->color.isEmpty() && color.isValid())
        {
            nameLabel->setStyleSheet(
                QStringLiteral("color: %1;").arg(color.name()));
        }
    }
    else
    {
        nameLabel->setText(QStringLiteral("User %1").arg(redemption.userId));
    }
    userLayout->addWidget(nameLabel);

    if (this->selectedRewardId_.isEmpty() && !redemption.rewardTitle.isEmpty())
    {
        auto *rewardLabel = new QLabel(redemption.rewardTitle, row);
        rewardLabel->setObjectName("RewardQueueMuted");
        rewardLabel->setTextFormat(Qt::PlainText);
        userLayout->addWidget(rewardLabel);
    }
    userLayout->addStretch(1);

    auto *timeLabel = new QLabel(formatAge(redemption.timestamp), row);
    timeLabel->setObjectName("RewardQueueMuted");
    timeLabel->setTextFormat(Qt::PlainText);
    if (redemption.timestamp.isValid())
    {
        timeLabel->setToolTip(QLocale().toString(
            redemption.timestamp.toLocalTime(), QLocale::ShortFormat));
    }
    userLayout->addWidget(timeLabel);
    this->timeLabels_.emplace_back(timeLabel, redemption.timestamp);

    if (!redemption.input.isEmpty())
    {
        auto *messageLabel = new QLabel(redemption.input, row);
        messageLabel->setTextFormat(Qt::PlainText);
        messageLabel->setWordWrap(true);
        messageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        textLayout->addWidget(messageLabel);
    }

    auto *completeButton = new QPushButton("Complete", row);
    completeButton->setObjectName("RewardQueueCompleteButton");
    completeButton->setCursor(Qt::PointingHandCursor);
    completeButton->setEnabled(!this->actionInFlight_);
    QObject::connect(completeButton, &QPushButton::clicked, this,
                     [this, id = redemption.id] {
                         this->updateRedemptions({id}, true);
                     });
    rowLayout->addWidget(completeButton, 0, Qt::AlignVCenter);

    auto *rejectButton = new QPushButton("Reject", row);
    rejectButton->setObjectName("RewardQueueButton");
    rejectButton->setCursor(Qt::PointingHandCursor);
    rejectButton->setEnabled(!this->actionInFlight_);
    QObject::connect(rejectButton, &QPushButton::clicked, this,
                     [this, id = redemption.id] {
                         this->updateRedemptions({id}, false);
                     });
    rowLayout->addWidget(rejectButton, 0, Qt::AlignVCenter);

    return row;
}

void RewardQueueDialog::updateActions()
{
    const auto idle = !this->loading_ && !this->actionInFlight_;
    const auto selectedCount = int(this->selected_.size());
    const auto hasRedemptions = !this->redemptions_.isEmpty();

    this->completeSelectedButton_->setText(
        QStringLiteral("Complete (%1)").arg(selectedCount));
    this->rejectSelectedButton_->setText(
        QStringLiteral("Reject (%1)").arg(selectedCount));
    this->completeSelectedButton_->setEnabled(idle && selectedCount > 0);
    this->rejectSelectedButton_->setEnabled(idle && selectedCount > 0);
    this->completeAllButton_->setEnabled(idle && hasRedemptions);
    this->rejectAllButton_->setEnabled(idle && hasRedemptions);
    this->loadMoreButton_->setEnabled(idle);
    this->loadMoreButton_->setVisible(this->hasNextPage_);
    this->sortButton_->setEnabled(idle);

    const QSignalBlocker blocker(this->selectAllCheckBox_);
    this->selectAllCheckBox_->setEnabled(hasRedemptions);
    this->selectAllCheckBox_->setChecked(
        hasRedemptions && selectedCount == this->redemptions_.size());
}

void RewardQueueDialog::toggleSortOrder()
{
    if (this->loading_ || this->actionInFlight_)
    {
        return;
    }

    this->newestFirst_ = !this->newestFirst_;
    this->updateSortButton();
    this->selected_.clear();
    this->redemptions_.clear();
    this->nextCursor_.clear();
    this->hasNextPage_ = false;
    this->extraPagesLoaded_ = false;
    this->updateAllStatus_.reset();
    this->rebuildList();

    this->reloadTimer_.stop();
    ++this->generation_;
    this->quietLoad_ = false;
    this->setStatus("Loading reward requests...");
    this->loadRedemptions(false);
}

void RewardQueueDialog::updateSortButton()
{
    if (this->newestFirst_)
    {
        this->sortButton_->setText("Sort by newest");
        this->sortButton_->setToolTip("Showing the most recent requests first");
        return;
    }

    this->sortButton_->setText("Sort by oldest");
    this->sortButton_->setToolTip("Showing the oldest requests first");
}

void RewardQueueDialog::selectReward(const QString &rewardId)
{
    this->selectedRewardId_ = rewardId;
    this->selected_.clear();
    this->redemptions_.clear();
    this->nextCursor_.clear();
    this->hasNextPage_ = false;
    this->extraPagesLoaded_ = false;
    this->updateAllStatus_.reset();
    this->rebuildSidebar();
    this->rebuildList();

    this->reloadTimer_.stop();
    ++this->generation_;
    this->quietLoad_ = false;
    this->setStatus("Loading reward requests...");
    this->loadRedemptions(false);
}

void RewardQueueDialog::updateRedemptions(const QStringList &ids, bool fulfill)
{
    if (ids.isEmpty() || this->actionInFlight_)
    {
        return;
    }

    const auto token = this->authTokenOrMessage("managing the reward queue");
    if (token.isEmpty())
    {
        this->updateAllStatus_.reset();
        return;
    }

    this->actionInFlight_ = true;
    this->inFlightIds_ = ids;
    for (const auto &id : ids)
    {
        this->resolvedIds_.insert(id);
        this->resolveLocally(id, {});
    }
    this->setStatus(fulfill ? "Completing requests..."
                            : "Rejecting requests...");
    this->rebuildSidebar();
    this->rebuildList();
    this->sendStatusUpdate(ids, fulfill, token);
}

void RewardQueueDialog::sendStatusUpdate(const QStringList &ids, bool fulfill,
                                         const QString &token)
{
    const auto batch = ids.mid(0, STATUS_UPDATE_BATCH_SIZE);
    const auto remaining = ids.mid(STATUS_UPDATE_BATCH_SIZE);

    QPointer<RewardQueueDialog> self = this;
    auto successCallback = [self, remaining, fulfill, token] {
        if (!self)
        {
            return;
        }
        if (remaining.isEmpty())
        {
            self->finishStatusUpdate({});
            return;
        }
        self->sendStatusUpdate(remaining, fulfill, token);
    };
    auto failureCallback = [self](const QString &error) {
        if (!self)
        {
            return;
        }
        self->finishStatusUpdate(MoltorinoAuth::normalizeAuthError(
            "managing the reward queue", error));
    };

    if (batch.size() == 1)
    {
        TwitchGql::updateRewardRedemptionStatus(this->channelId_, batch.first(),
                                                fulfill, token, successCallback,
                                                failureCallback);
        return;
    }

    TwitchGql::updateRewardRedemptionStatuses(this->channelId_, batch, fulfill,
                                              token, successCallback,
                                              failureCallback);
}

void RewardQueueDialog::finishStatusUpdate(const QString &error)
{
    this->actionInFlight_ = false;
    if (!error.isEmpty())
    {
        for (const auto &id : this->inFlightIds_)
        {
            this->resolvedIds_.remove(id);
        }
        this->inFlightIds_.clear();
        this->updateAllStatus_.reset();
        this->pendingError_ = error;
        this->reload();
        return;
    }

    this->inFlightIds_.clear();
    if (this->updateAllStatus_ ||
        (this->redemptions_.isEmpty() && this->hasNextPage_))
    {
        this->reload();
        return;
    }

    this->setIdleStatus();
    this->rebuildList();
}

void RewardQueueDialog::updateAll(bool fulfill)
{
    if (this->redemptions_.isEmpty() || this->loading_ || this->actionInFlight_)
    {
        return;
    }

    const auto scope =
        this->selectedRewardId_.isEmpty()
            ? QStringLiteral("every pending request in this channel")
            : QStringLiteral("every pending request for \"%1\"")
                  .arg(this->titleLabel_->text());
    const auto question =
        fulfill
            ? QStringLiteral("Complete %1?").arg(scope)
            : QStringLiteral("Reject %1? Rejected requests refund the points.")
                  .arg(scope);
    if (QMessageBox::question(this, this->windowTitle(), question) !=
        QMessageBox::Yes)
    {
        return;
    }

    this->updateAllStatus_ = fulfill;
    this->updateAllRounds_ = 1;
    this->updateRedemptions(this->loadedIds(), fulfill);
}

void RewardQueueDialog::queueReload()
{
    if (!this->reloadTimer_.isActive())
    {
        this->reloadTimer_.start();
    }
}

void RewardQueueDialog::setStatus(const QString &text, bool error)
{
    this->statusIsError_ = error;
    this->statusLabel_->setText(text);
    this->statusLabel_->setStyleSheet(error ? QStringLiteral("color: #ff9e9e;")
                                            : QString());
}

QString RewardQueueDialog::authTokenOrMessage(const QString &action)
{
    QString authError;
    const auto auth = MoltorinoAuth::resolveModerationToken(
        this->channelId_, this->channelLogin_, &authError);
    if (auth.hasToken())
    {
        return auth.token;
    }

    this->setStatus(authError.isEmpty()
                        ? MoltorinoAuth::authRequiredMessage(action)
                        : authError,
                    true);
    return {};
}

QStringList RewardQueueDialog::loadedIds() const
{
    QStringList ids;
    ids.reserve(this->redemptions_.size());
    for (const auto &redemption : this->redemptions_)
    {
        ids.push_back(redemption.id);
    }
    return ids;
}

QStringList RewardQueueDialog::selectedIds() const
{
    QStringList ids;
    for (const auto &redemption : this->redemptions_)
    {
        if (this->selected_.contains(redemption.id))
        {
            ids.push_back(redemption.id);
        }
    }
    return ids;
}

void RewardQueueDialog::loadPixmap(
    const QString &url, std::function<void(const QPixmap &)> callback)
{
    if (url.isEmpty())
    {
        return;
    }

    const auto cached = this->pixmaps_.constFind(url);
    if (cached != this->pixmaps_.constEnd())
    {
        callback(*cached);
        return;
    }

    auto &pending = this->pendingPixmaps_[url];
    pending.push_back(std::move(callback));
    if (pending.size() > 1)
    {
        return;
    }

    QPointer<RewardQueueDialog> self = this;
    NetworkRequest(url)
        .cache()
        .onSuccess([self, url](const NetworkResult &result) {
            if (!self)
            {
                return;
            }

            QPixmap pixmap;
            const auto callbacks = self->pendingPixmaps_.take(url);
            if (!pixmap.loadFromData(result.getData()))
            {
                return;
            }

            self->pixmaps_.insert(url, pixmap);
            for (const auto &pendingCallback : callbacks)
            {
                pendingCallback(pixmap);
            }
        })
        .onError([self, url](const NetworkResult &) {
            if (self)
            {
                self->pendingPixmaps_.remove(url);
            }
        })
        .execute();
}

}  // namespace chatterino

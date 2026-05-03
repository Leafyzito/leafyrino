// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/SplitPinnedMessagePanel.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/MessageView.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitCommon.hpp"

#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMargins>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

using namespace chatterino;

constexpr int POLL_INTERVAL_MS = 5'000;

QString formatPinnedLine(const TwitchGql::PinnedChatMessage &m)
{
    if (m.text.trimmed().isEmpty())
    {
        return m.senderDisplayName.trimmed();
    }

    return QStringLiteral("%1: %2").arg(m.senderDisplayName.trimmed(),
                                        m.text.trimmed());
}

}  // namespace

namespace chatterino {

SplitPinnedMessagePanel::SplitPinnedMessagePanel(Split *split)
    : BaseWidget(split)
    , split_(split)
{
    this->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Minimum);
    this->hide();

    this->pollTimer_.setInterval(POLL_INTERVAL_MS);
    this->pollTimer_.setTimerType(Qt::CoarseTimer);
    QObject::connect(&this->pollTimer_, &QTimer::timeout, this, [this] {
        this->fetchPinned();
    });

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    auto *topRow = new QWidget(this);
    auto *hl = new QHBoxLayout(topRow);
    hl->setContentsMargins(6, 2, 6, 2);
    hl->setSpacing(4);

    this->iconLabel_ = new QLabel(topRow);
    this->iconLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    this->iconLabel_->setScaledContents(false);

    this->collapsedLabel_ = new QLabel(topRow);
    this->collapsedLabel_->setSizePolicy(QSizePolicy::MinimumExpanding,
                                         QSizePolicy::Preferred);
    this->collapsedLabel_->setWordWrap(false);

    this->dismissButton_ = new QPushButton(topRow);
    this->dismissButton_->setFlat(true);
    this->dismissButton_->setCursor(Qt::PointingHandCursor);
    this->dismissButton_->setFocusPolicy(Qt::NoFocus);
    this->dismissButton_->setText(QStringLiteral("×"));
    this->dismissButton_->setToolTip(
        QStringLiteral("Dismiss for this message"));
    QObject::connect(this->dismissButton_, &QPushButton::clicked, this, [this] {
        if (!this->current_.has_value())
        {
            return;
        }
        this->dismissedForPinnedMessageId_ = this->current_->id;
        this->hidePinnedUi();
    });

    this->expandButton_ = new QPushButton(topRow);
    this->expandButton_->setFlat(true);
    this->expandButton_->setCursor(Qt::PointingHandCursor);
    this->expandButton_->setFocusPolicy(Qt::NoFocus);
    QObject::connect(this->expandButton_, &QPushButton::clicked, this, [this] {
        this->expanded_ = !this->expanded_;
        this->updateExpandToggle();
    });

    hl->addWidget(this->iconLabel_, 0, Qt::AlignVCenter);
    hl->addWidget(this->collapsedLabel_, 1);
    hl->addWidget(this->expandButton_, 0, Qt::AlignRight | Qt::AlignVCenter);
    hl->addWidget(this->dismissButton_, 0, Qt::AlignRight | Qt::AlignVCenter);

    this->expandedWidget_ = new QWidget(this);
    auto *expandedLay = new QVBoxLayout(this->expandedWidget_);
    expandedLay->setContentsMargins(8, 0, 8, 6);
    expandedLay->setSpacing(0);

    this->expandedMessageView_ = new MessageView(this->expandedWidget_);
    expandedLay->addWidget(this->expandedMessageView_);

    mainLayout->addWidget(topRow);
    mainLayout->addWidget(this->expandedWidget_);

    this->installClickFocusesSplit(this);

    this->managedConnections_.addConnection(pajlada::Signals::ScopedConnection(
        this->expandedMessageView_->selectionChanged.connect([this] {
            this->split_->getChannelView().clearSelection();
        })));

    this->updateExpandToggle();

    getSettings()->showPinnedMessagePanel.connect(
        [this](const bool & /*enabled*/) {
            this->startOrStopTimer();
            if (!getSettings()->showPinnedMessagePanel ||
                this->split_->perSplitHidePinnedMessage())
            {
                this->hidePanel();
            }
            else
            {
                this->refresh();
            }
        },
        this->managedConnections_);

    this->managedConnections_.addConnection(pajlada::Signals::ScopedConnection(
        this->split_->focused.connect([this] {
            this->update();
            this->updateStyleSheets();
            this->refresh();
        })));
    this->managedConnections_.addConnection(pajlada::Signals::ScopedConnection(
        this->split_->focusLost.connect([this] {
            this->update();
            this->updateStyleSheets();
        })));

    this->managedConnections_.managedConnect(
        getApp()->getWindows()->wordFlagsChanged, [this] {
            if (this->expandedMessageView_ != nullptr)
            {
                this->expandedMessageView_->relayout();
            }
        });

    this->themeChangedEvent();
    this->scaleChangedEvent(this->scale());
}

void SplitPinnedMessagePanel::installClickFocusesSplit(QWidget *root)
{
    if (root == nullptr)
    {
        return;
    }
    root->installEventFilter(this);
    for (QObject *child : root->children())
    {
        if (auto *cw = qobject_cast<QWidget *>(child))
        {
            this->installClickFocusesSplit(cw);
        }
    }
}

bool SplitPinnedMessagePanel::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress)
    {
        const auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton)
        {
            this->split_->setFocus(Qt::MouseFocusReason);
        }
    }
    return BaseWidget::eventFilter(watched, event);
}

void SplitPinnedMessagePanel::clearExpandedMessageSelection()
{
    if (this->expandedMessageView_ != nullptr)
    {
        this->expandedMessageView_->clearSelection();
    }
}

void SplitPinnedMessagePanel::setTwitchChannel(TwitchChannel *channel)
{
    this->channelConnections_.clear();
    this->twitchChannel_ = channel;
    this->pollTimer_.stop();
    this->inFlight_ = false;
    this->current_.reset();
    this->hidePanel();

    if (channel == nullptr)
    {
        return;
    }

    this->channelConnections_.managedConnect(channel->roomIdAssigned, [this] {
        this->startOrStopTimer();
        this->refresh();
    });
    this->channelConnections_.managedConnect(channel->destroyed, [this] {
        this->setTwitchChannel(nullptr);
    });

    this->startOrStopTimer();
    this->refresh();
}

void SplitPinnedMessagePanel::refresh()
{
    this->fetchPinned();
}

void SplitPinnedMessagePanel::recoverDismissedPanel()
{
    this->dismissedForPinnedMessageId_.clear();
    this->refresh();
}

void SplitPinnedMessagePanel::startOrStopTimer()
{
    this->pollTimer_.stop();

    if (!getSettings()->showPinnedMessagePanel ||
        this->split_->perSplitHidePinnedMessage())
    {
        return;
    }
    if (this->twitchChannel_ == nullptr)
    {
        return;
    }
    if (getApp()->getAccounts()->twitch.getCurrent()->isAnon())
    {
        return;
    }
    if (this->twitchChannel_->roomId().isEmpty())
    {
        return;
    }

    this->pollTimer_.start();
}

void SplitPinnedMessagePanel::fetchPinned()
{
    if (!getSettings()->showPinnedMessagePanel ||
        this->split_->perSplitHidePinnedMessage())
    {
        this->hidePanel();
        return;
    }

    auto *tc = this->twitchChannel_;
    if (tc == nullptr)
    {
        return;
    }

    if (getApp()->getAccounts()->twitch.getCurrent()->isAnon())
    {
        this->hidePanel();
        return;
    }

    const auto roomId = tc->roomId();
    if (roomId.isEmpty())
    {
        return;
    }

    if (this->inFlight_)
    {
        return;
    }
    this->inFlight_ = true;

    const QString fetchRoomId = roomId;
    const auto acc = getApp()->getAccounts()->twitch.getCurrent();

    TwitchGql::fetchPinnedChatMessage(
        fetchRoomId, 1, acc->getOAuthToken(), acc->getOAuthClient(), this,
        [this,
         fetchRoomId](std::optional<TwitchGql::PinnedChatMessage> pinned) {
            this->inFlight_ = false;
            if (this->twitchChannel_ == nullptr ||
                this->twitchChannel_->roomId() != fetchRoomId)
            {
                return;
            }
            if (!pinned.has_value())
            {
                this->current_.reset();
                this->hidePanel();
                return;
            }
            if (!pinned->id.isEmpty() &&
                pinned->id == this->dismissedForPinnedMessageId_)
            {
                this->current_.reset();
                this->hidePinnedUi();
                return;
            }
            this->current_ = std::move(pinned);
            this->updateText();
            this->show();
            this->updateStyleSheets();
        },
        [this, fetchRoomId](const QString &) {
            this->inFlight_ = false;
            if (this->twitchChannel_ != nullptr &&
                this->twitchChannel_->roomId() == fetchRoomId)
            {
                this->hidePanel();
            }
        });
}

void SplitPinnedMessagePanel::updatePinIcon()
{
    if (this->iconLabel_ == nullptr || this->theme == nullptr)
    {
        return;
    }

    const int px = splitHeaderIconColumnWidth(this->scale());
    const QString iconPath =
        this->theme->isLightTheme()
            ? QStringLiteral(":/buttons/pinDisabled-lightMode.svg")
            : QStringLiteral(":/buttons/pinDisabled-darkMode.svg");
    const QPixmap pm = QIcon(iconPath).pixmap(px, px);
    if (!pm.isNull())
    {
        this->iconLabel_->setPixmap(pm);
    }
    this->iconLabel_->setFixedWidth(px);
    this->iconLabel_->setToolTip(QStringLiteral("Pinned message"));
}

void SplitPinnedMessagePanel::updateExpandToggle()
{
    if (this->expandedWidget_ != nullptr)
    {
        this->expandedWidget_->setVisible(this->expanded_);
    }
    if (this->expandButton_ != nullptr)
    {
        this->expandButton_->setText(this->expanded_ ? QStringLiteral("▼")
                                                     : QStringLiteral("▶"));
        this->expandButton_->setToolTip(this->expanded_
                                            ? QStringLiteral("Collapse")
                                            : QStringLiteral("Expand"));
    }
    this->refreshTopRowText();
    this->updateExpandedMessageWidth();
}

void SplitPinnedMessagePanel::updateText()
{
    if (!this->current_.has_value())
    {
        return;
    }
    this->fullPinnedText_ = formatPinnedLine(*this->current_);
    if (this->expandedMessageView_ != nullptr &&
        this->twitchChannel_ != nullptr)
    {
        const auto msg = MessageBuilder::makePinnedChatPreviewMessage(
            this->twitchChannel_, *this->current_);
        this->expandedMessageView_->setFullMessage(msg);
        this->updateExpandedMessageWidth();
    }
    this->refreshTopRowText();
}

void SplitPinnedMessagePanel::refreshTopRowText()
{
    if (this->collapsedLabel_ == nullptr || this->fullPinnedText_.isEmpty())
    {
        return;
    }

    if (this->expanded_)
    {
        this->collapsedLabel_->setText(QStringLiteral("Pinned message"));
        return;
    }

    this->updateCollapsedElide();
}

void SplitPinnedMessagePanel::updateCollapsedElide()
{
    if (this->collapsedLabel_ == nullptr || this->fullPinnedText_.isEmpty() ||
        this->expanded_)
    {
        return;
    }

    // `QFontMetrics::elidedText` takes a maximum width in *pixels*. Using only
    // `collapsedLabel_->width()` is wrong when layout has not run yet (width 0
    // → fallback could be `max(40, panelWidth - 72)` with panelWidth still 0,
    // yielding ~40px and a "t..."), or when the label briefly reports a tiny
    // width. Derive space from the top row: row width minus icon, expand,
    // dismiss, margins, and spacers.
    int avail = 0;
    auto *topRow = this->collapsedLabel_->parentWidget();
    auto *hl = topRow ? qobject_cast<QHBoxLayout *>(topRow->layout()) : nullptr;
    const int rowW = topRow ? topRow->width() : this->width();

    if (rowW > 0 && this->iconLabel_ != nullptr &&
        this->dismissButton_ != nullptr && this->expandButton_ != nullptr)
    {
        const QMargins cm = hl ? hl->contentsMargins() : QMargins{};
        const int spacing = hl ? hl->spacing() : 4;
        const int iconW = this->iconLabel_->width() > 0
                              ? this->iconLabel_->width()
                              : splitHeaderIconColumnWidth(this->scale());
        const int dismissW = this->dismissButton_->width() > 0
                                 ? this->dismissButton_->width()
                                 : this->dismissButton_->sizeHint().width();
        const int expandW = this->expandButton_->width() > 0
                                ? this->expandButton_->width()
                                : this->expandButton_->sizeHint().width();
        avail = rowW - cm.left() - cm.right() - iconW - dismissW - expandW -
                3 * spacing;
    }
    else if (this->width() > 0)
    {
        avail = this->width() - 100;
    }
    else
    {
        avail = this->collapsedLabel_->width();
    }

    avail = std::max(40, avail);

    const QFontMetrics fm(this->collapsedLabel_->font());
    const QString elided =
        fm.elidedText(this->fullPinnedText_, Qt::ElideRight, avail);
    this->collapsedLabel_->setText(elided.isEmpty() ? this->fullPinnedText_
                                                    : elided);
}

void SplitPinnedMessagePanel::hidePinnedUi()
{
    this->expanded_ = false;
    this->updateExpandToggle();
    this->current_.reset();
    this->fullPinnedText_.clear();
    if (this->collapsedLabel_ != nullptr)
    {
        this->collapsedLabel_->clear();
    }
    if (this->expandedMessageView_ != nullptr)
    {
        this->expandedMessageView_->setFullMessage(nullptr);
    }
    this->hide();
}

void SplitPinnedMessagePanel::hidePanel()
{
    this->dismissedForPinnedMessageId_.clear();
    this->hidePinnedUi();
}

void SplitPinnedMessagePanel::updateStyleSheets()
{
    if (this->theme == nullptr)
    {
        return;
    }

    const auto &text = this->theme->splits.header.text;
    const auto textCss = text.name(QColor::HexArgb);

    const QString labelStyle =
        QStringLiteral("QLabel { color: %1; }").arg(textCss);

    if (this->collapsedLabel_ != nullptr)
    {
        this->collapsedLabel_->setStyleSheet(labelStyle);
    }
    const QString btnStyle =
        QStringLiteral("QPushButton { color: %1; text-decoration: underline; "
                       "border: none; "
                       "padding: 2px 6px; } "
                       "QPushButton:hover { color: %2; }")
            .arg(textCss, textCss);
    if (this->dismissButton_ != nullptr)
    {
        this->dismissButton_->setStyleSheet(btnStyle);
    }
    if (this->expandButton_ != nullptr)
    {
        this->expandButton_->setStyleSheet(btnStyle);
    }
}

void SplitPinnedMessagePanel::themeChangedEvent()
{
    BaseWidget::themeChangedEvent();
    this->updateStyleSheets();
    this->updatePinIcon();
}

void SplitPinnedMessagePanel::scaleChangedEvent(float scale)
{
    BaseWidget::scaleChangedEvent(scale);

    const int fs = static_cast<int>(9 * std::max(1.0f, scale));
    QFont f = this->font();
    f.setPointSize(std::max(8, fs));
    f.setBold(true);
    if (this->collapsedLabel_ != nullptr)
    {
        this->collapsedLabel_->setFont(f);
    }
    if (this->expandedMessageView_ != nullptr)
    {
        this->expandedMessageView_->relayout();
    }

    this->updatePinIcon();
    if (auto *lay = this->expandedWidget_ != nullptr
                        ? this->expandedWidget_->layout()
                        : nullptr)
    {
        const int pad = static_cast<int>(4 * scale);
        lay->setContentsMargins(8, pad, 8, pad + 2);
    }

    this->updateCollapsedElide();
}

void SplitPinnedMessagePanel::paintEvent(QPaintEvent * /*event*/)
{
    if (this->theme == nullptr)
    {
        return;
    }

    QPainter painter(this);
    QColor bg = this->theme->splits.background;
    QColor border = this->theme->splits.header.border;
    if (this->split_->hasFocus())
    {
        border = this->theme->splits.header.focusedBorder;
    }

    painter.fillRect(this->rect(), bg);
    painter.setPen(border);
    painter.drawRect(0, 0, this->width() - 1, this->height() - 1);
}

void SplitPinnedMessagePanel::resizeEvent(QResizeEvent *event)
{
    BaseWidget::resizeEvent(event);
    this->updateCollapsedElide();
    this->updateExpandedMessageWidth();
}

void SplitPinnedMessagePanel::showEvent(QShowEvent *event)
{
    BaseWidget::showEvent(event);
    QTimer::singleShot(0, this, [this] {
        this->updateCollapsedElide();
        this->updateExpandedMessageWidth();
    });
}

void SplitPinnedMessagePanel::updateExpandedMessageWidth()
{
    if (this->expandedMessageView_ == nullptr ||
        this->expandedWidget_ == nullptr || !this->expanded_)
    {
        return;
    }

    int margins = 16;
    if (auto *lay = this->expandedWidget_->layout(); lay != nullptr)
    {
        const auto m = lay->contentsMargins();
        margins = m.left() + m.right();
    }

    const int w = std::max(1, this->expandedWidget_->width() - margins);
    this->expandedMessageView_->setWidth(w);
}

}  // namespace chatterino

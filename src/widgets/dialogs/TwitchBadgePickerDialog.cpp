// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/TwitchBadgePickerDialog.hpp"

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS

#    include "messages/Image.hpp"
#    include "providers/moltorino/MoltorinoAuth.hpp"
#    include "providers/twitch/TwitchChannel.hpp"
#    include "providers/twitch/api/TwitchGql.hpp"
#    include "singletons/Fonts.hpp"
#    include "singletons/Theme.hpp"
#    include "widgets/buttons/Button.hpp"
#    include "widgets/buttons/SvgButton.hpp"
#    include "widgets/helper/Line.hpp"

#    include <QCursor>
#    include <QGridLayout>
#    include <QHBoxLayout>
#    include <QLabel>
#    include <QPainter>
#    include <QPushButton>
#    include <QResizeEvent>
#    include <QScrollArea>
#    include <QScrollBar>
#    include <QShowEvent>
#    include <QTimer>
#    include <QVBoxLayout>

namespace chatterino {

namespace {

constexpr QSize DEFAULT_DIALOG_SIZE(320, 400);
constexpr int BADGE_GRID_COLUMNS = 4;
constexpr int BADGE_SIZE_PX = 52;

class BadgeTileButton final : public QPushButton
{
public:
    BadgeTileButton(const GqlBadge &badge, QWidget *parent)
        : QPushButton(parent)
        , badge_(badge)
        , image_(badge.image2x.isEmpty()
                     ? Image::getEmpty()
                     : Image::fromUrl(Url{badge.image2x}, 1,
                                      {BADGE_SIZE_PX, BADGE_SIZE_PX}))
    {
        this->setCursor(Qt::PointingHandCursor);
        this->setFocusPolicy(Qt::StrongFocus);
        this->setAttribute(Qt::WA_Hover, true);
        this->setFlat(true);
        this->setFixedSize(BADGE_SIZE_PX + 8, BADGE_SIZE_PX + 8);
        this->setToolTip(badge.title);
    }

    const GqlBadge &badge() const
    {
        return this->badge_;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

        const auto *theme = getApp()->getThemes();
        auto bg = theme->splits.input.background;
        auto border = theme->splits.header.border;

        if (this->underMouse())
        {
            bg = theme->isLightTheme() ? bg.darker(105) : bg.lighter(115);
            border = theme->splits.header.focusedBorder;
        }
        if (this->isDown())
            bg = theme->isLightTheme() ? bg.darker(112) : bg.lighter(125);

        const auto rect = this->rect().adjusted(0, 0, -1, -1);
        painter.setPen(QPen(border, 1));
        painter.setBrush(bg);
        painter.drawRoundedRect(rect, 3, 3);

        const int pad = 4;
        const QRect imgRect(pad, pad, rect.width() - pad * 2,
                            rect.height() - pad * 2);

        if (auto pixmap = this->image_->pixmapOrLoad())
            painter.drawPixmap(imgRect, *pixmap, pixmap->rect());
        else
        {
            auto muted = theme->window.text;
            muted.setAlpha(50);
            painter.setBrush(muted);
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(imgRect, 2, 2);
        }

        if (this->hasFocus())
        {
            auto focus = theme->window.text;
            focus.setAlpha(200);
            painter.setPen(QPen(focus, 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(rect.adjusted(1, 1, -1, -1), 3, 3);
        }
    }

private:
    GqlBadge badge_;
    ImagePtr image_;
};

}  // namespace

std::vector<QPointer<TwitchBadgePickerDialog>>
    TwitchBadgePickerDialog::activeDialogs_;

TwitchBadgePickerDialog::TwitchBadgePickerDialog(TwitchChannel *channel,
                                                 QWidget *parent)
    : DraggablePopup(true, parent)
    , channel_(channel)
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setObjectName("TwitchBadgePickerDialog");
    this->setWindowTitle("Select Badge");
    this->setScaleIndependentSize(DEFAULT_DIALOG_SIZE);

    auto *container = this->getLayoutContainer();
    this->mainLayout_ = new QVBoxLayout(container);
    this->mainLayout_->setSpacing(0);
    this->mainLayout_->setContentsMargins(0, 0, 0, 0);

    // Header
    this->headerWidget_ = new QWidget(container);
    auto *headerLayout = new QHBoxLayout(this->headerWidget_);
    headerLayout->setContentsMargins(8, 4, 8, 4);
    headerLayout->setSpacing(4);

    this->headerTitleLabel_ = new QLabel("Select Badge", this->headerWidget_);
    this->headerTitleLabel_->setObjectName("TwitchBadgePickerTitle");
    headerLayout->addWidget(this->headerTitleLabel_);
    headerLayout->addStretch(1);

    this->pinButton_ = this->createPinButton();
    headerLayout->addWidget(this->pinButton_);

    this->closeButton_ = new SvgButton(
        {.dark = ":/buttons/cancel.svg", .light = ":/buttons/cancelDark.svg"},
        this, QSize{3, 3});
    this->closeButton_->setScaleIndependentSize(18, 18);
    this->closeButton_->setToolTip("Close");
    this->closeButton_->setCursor(Qt::PointingHandCursor);
    QObject::connect(this->closeButton_, &Button::leftClicked, this,
                     &QWidget::close);
    headerLayout->addWidget(this->closeButton_);
    this->mainLayout_->addWidget(this->headerWidget_);
    this->mainLayout_->addWidget(new Line(false));

    // Tab row
    auto *tabRow = new QHBoxLayout();
    tabRow->setSpacing(4);
    tabRow->setContentsMargins(8, 4, 8, 0);

    this->globalTabButton_ = new QPushButton("Global Badge", container);
    this->globalTabButton_->setObjectName("TwitchBadgePickerTab");
    this->globalTabButton_->setCheckable(true);
    this->globalTabButton_->setChecked(true);
    this->globalTabButton_->setCursor(Qt::PointingHandCursor);
    QObject::connect(this->globalTabButton_, &QPushButton::clicked, this,
                     [this] {
                         this->view_ = View::GlobalBadges;
                         this->globalTabButton_->setChecked(true);
                         this->channelTabButton_->setChecked(false);
                         this->rebuildContent();
                     });

    this->channelTabButton_ = new QPushButton("Channel Badge", container);
    this->channelTabButton_->setObjectName("TwitchBadgePickerTab");
    this->channelTabButton_->setCheckable(true);
    this->channelTabButton_->setChecked(false);
    this->channelTabButton_->setCursor(Qt::PointingHandCursor);
    QObject::connect(this->channelTabButton_, &QPushButton::clicked, this,
                     [this] {
                         this->view_ = View::ChannelBadges;
                         this->globalTabButton_->setChecked(false);
                         this->channelTabButton_->setChecked(true);
                         this->rebuildContent();
                     });

    tabRow->addWidget(this->globalTabButton_);
    tabRow->addWidget(this->channelTabButton_);
    tabRow->addStretch(1);
    this->mainLayout_->addLayout(tabRow);

    // Scroll area
    this->scrollArea_ = new QScrollArea(container);
    this->scrollArea_->setFrameShape(QFrame::NoFrame);
    this->scrollArea_->setWidgetResizable(true);
    this->scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->scrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    this->mainLayout_->addWidget(this->scrollArea_, 1);

    this->contentWidget_ = new QWidget();
    this->contentLayout_ = new QVBoxLayout(this->contentWidget_);
    this->contentLayout_->setContentsMargins(8, 8, 8, 8);
    this->contentLayout_->setSpacing(6);
    this->scrollArea_->setWidget(this->contentWidget_);

    this->refreshStyle();
    this->rebuildContent();
}

void TwitchBadgePickerDialog::showDialog(TwitchChannel *channel,
                                         QWidget *parent)
{
    if (!channel)
        return;

    for (auto it = activeDialogs_.begin(); it != activeDialogs_.end();)
    {
        if (it->isNull())
        {
            it = activeDialogs_.erase(it);
            continue;
        }
        if ((*it)->channel_ == channel)
        {
            (*it)->raise();
            (*it)->activateWindow();
            (*it)->loadBadges(true);
            return;
        }
        ++it;
    }

    auto *dialog = new TwitchBadgePickerDialog(channel, parent);
    activeDialogs_.push_back(dialog);

    QPoint center = QCursor::pos();
    if (parent && parent->window())
        center = parent->window()->geometry().center();

    dialog->show();
    const auto size = dialog->size();
    dialog->showAndMoveTo(
        center - QPoint(size.width() / 2, size.height() / 2),
        widgets::BoundsChecking::DesiredPosition);
    dialog->raise();
    dialog->activateWindow();
    dialog->loadBadges(false);
}

void TwitchBadgePickerDialog::themeChangedEvent()
{
    DraggablePopup::themeChangedEvent();
    this->refreshStyle();
}

void TwitchBadgePickerDialog::scaleChangedEvent(float scale)
{
    DraggablePopup::scaleChangedEvent(scale);
    this->refreshStyle();
    this->applySizeConstraints();
}

void TwitchBadgePickerDialog::resizeEvent(QResizeEvent *event)
{
    DraggablePopup::resizeEvent(event);
}

void TwitchBadgePickerDialog::showEvent(QShowEvent *event)
{
    DraggablePopup::showEvent(event);
    if (!this->initialFetchDone_)
    {
        QTimer::singleShot(0, this, [this] {
            if (!this->initialFetchDone_)
                this->loadBadges(false);
        });
    }
}

void TwitchBadgePickerDialog::loadBadges(bool force)
{
    if (this->badgesLoading_ && !force)
        return;

    const auto token = this->authTokenOrMessage();
    if (token.isEmpty())
    {
        this->initialFetchDone_ = true;
        this->rebuildContent();
        return;
    }

    this->initialFetchDone_ = true;
    this->badgesLoading_ = true;
    this->setStatus("Loading badges...");
    this->rebuildContent();

    QPointer<TwitchBadgePickerDialog> self = this;
    const auto channelLogin = this->channel_->getName();

    TwitchGql::getChatSettingsBadges(
        channelLogin, token,
        [self](GqlChatSettingsBadges badges) {
            if (!self)
                return;
            self->badgesLoading_ = false;
            self->badgesLoaded_ = true;
            self->badges_ = std::move(badges);
            self->setStatus({});
            self->rebuildContent();
        },
        [self](const QString &error) {
            if (!self)
                return;
            self->badgesLoading_ = false;
            self->setStatus(
                MoltorinoAuth::normalizeAuthError("loading badges", error),
                true);
            self->rebuildContent();
        });
}

void TwitchBadgePickerDialog::rebuildContent()
{
    this->clearContent();

    this->statusLabel_ = new QLabel(this->contentWidget_);
    this->statusLabel_->setObjectName("TwitchBadgePickerStatus");
    this->statusLabel_->setWordWrap(true);
    this->statusLabel_->setAlignment(Qt::AlignCenter);
    this->statusLabel_->hide();
    this->contentLayout_->addWidget(this->statusLabel_);
    this->setStatus(this->statusText_, this->statusIsError_);

    if (this->badgesLoading_)
    {
        this->setStatus("Loading badges...");
        this->contentLayout_->addStretch(1);
        return;
    }

    if (this->view_ == View::GlobalBadges)
        this->rebuildGlobalBadges();
    else
        this->rebuildChannelBadges();

    this->contentLayout_->addStretch(1);
    this->applySizeConstraints();
}

void TwitchBadgePickerDialog::rebuildGlobalBadges()
{
    const auto &available = this->badges_.availableGlobal;
    const auto &selected = this->badges_.selectedGlobalBadge;

    if (available.isEmpty())
    {
        if (!(this->statusIsError_ && !this->statusText_.isEmpty()))
            this->setStatus("No global badges available.");
        return;
    }

    auto *noBadge = new QPushButton("✕  No Badge", this->contentWidget_);
    noBadge->setObjectName("TwitchBadgePickerNoBadge");
    noBadge->setCursor(Qt::PointingHandCursor);
    noBadge->setEnabled(!this->actionInFlight_);
    QObject::connect(noBadge, &QPushButton::clicked, this,
                     [this] { this->selectGlobal({}); });
    this->contentLayout_->addWidget(noBadge);

    auto *label = new QLabel("Your Badges", this->contentWidget_);
    label->setObjectName("TwitchBadgePickerSectionLabel");
    this->contentLayout_->addWidget(label);

    auto *gridWidget = new QWidget(this->contentWidget_);
    auto *grid = new QGridLayout(gridWidget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(4);

    int row = 0;
    int col = 0;
    for (const auto &badge : available)
    {
        auto *tile = new BadgeTileButton(badge, gridWidget);
        tile->setEnabled(!this->actionInFlight_);

        if (!selected.setID.isEmpty() && badge.setID == selected.setID &&
            badge.version == selected.version)
            tile->setStyleSheet("border: 2px solid #9146ff; border-radius: 3px;");

        QObject::connect(tile, &QPushButton::clicked, this,
                         [this, badge] { this->selectGlobal(badge); });
        grid->addWidget(tile, row, col);
        if (++col >= BADGE_GRID_COLUMNS)
        {
            col = 0;
            ++row;
        }
    }

    this->contentLayout_->addWidget(gridWidget);
}

void TwitchBadgePickerDialog::rebuildChannelBadges()
{
    const auto &available = this->badges_.availableChannel;
    const auto &selected = this->badges_.selectedChannelBadge;

    if (available.isEmpty())
    {
        if (!(this->statusIsError_ && !this->statusText_.isEmpty()))
            this->setStatus("No channel-specific badges available.");
        return;
    }

    auto *label = new QLabel(
        QStringLiteral("Badges for #%1").arg(this->channel_->getName()),
        this->contentWidget_);
    label->setObjectName("TwitchBadgePickerSectionLabel");
    this->contentLayout_->addWidget(label);

    auto *gridWidget = new QWidget(this->contentWidget_);
    auto *grid = new QGridLayout(gridWidget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(4);

    int row = 0;
    int col = 0;
    for (const auto &badge : available)
    {
        auto *tile = new BadgeTileButton(badge, gridWidget);
        tile->setEnabled(!this->actionInFlight_);

        if (!selected.setID.isEmpty() && badge.setID == selected.setID &&
            badge.version == selected.version)
            tile->setStyleSheet("border: 2px solid #9146ff; border-radius: 3px;");

        QObject::connect(tile, &QPushButton::clicked, this,
                         [this, badge] { this->selectChannel(badge); });
        grid->addWidget(tile, row, col);
        if (++col >= BADGE_GRID_COLUMNS)
        {
            col = 0;
            ++row;
        }
    }

    this->contentLayout_->addWidget(gridWidget);
}

void TwitchBadgePickerDialog::clearContent()
{
    this->statusLabel_ = nullptr;
    while (auto *item = this->contentLayout_->takeAt(0))
    {
        if (auto *widget = item->widget())
        {
            widget->hide();
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete item;
    }
}

void TwitchBadgePickerDialog::setStatus(const QString &text, bool error)
{
    this->statusText_ = text;
    this->statusIsError_ = error;
    if (!this->statusLabel_)
        return;
    this->statusLabel_->setText(text);
    this->statusLabel_->setVisible(!text.isEmpty());
    auto muted = this->theme->window.text;
    muted.setAlpha(150);
    const auto color =
        error ? QStringLiteral("#ff9e9e") : muted.name(QColor::HexArgb);
    this->statusLabel_->setStyleSheet(
        QStringLiteral("color: %1;").arg(color));
}

void TwitchBadgePickerDialog::selectGlobal(const GqlBadge &badge)
{
    if (badge.setID.isEmpty())
        return;

    const auto token = this->authTokenOrMessage();
    if (token.isEmpty())
        return;

    this->actionInFlight_ = true;
    this->rebuildContent();

    QPointer<TwitchBadgePickerDialog> self = this;
    TwitchGql::selectGlobalBadge(
        badge.setID, badge.version, token,
        [self, badge] {
            if (!self)
                return;
            self->actionInFlight_ = false;
            self->badges_.selectedGlobalBadge = badge;
            self->channel_->addSystemMessage(
                QStringLiteral("Global badge set to: %1").arg(badge.title));
            self->rebuildContent();
        },
        [self](const QString &error) {
            if (!self)
                return;
            self->actionInFlight_ = false;
            self->setStatus(
                MoltorinoAuth::normalizeAuthError("selecting badge", error),
                true);
            self->rebuildContent();
        });
}

void TwitchBadgePickerDialog::selectChannel(const GqlBadge &badge)
{
    const auto token = this->authTokenOrMessage();
    if (token.isEmpty())
        return;

    this->actionInFlight_ = true;
    this->rebuildContent();

    QPointer<TwitchBadgePickerDialog> self = this;
    const auto channelId = this->channel_->roomId();

    TwitchGql::selectChannelBadge(
        badge.setID, badge.version, channelId, token,
        [self, badge] {
            if (!self)
                return;
            self->actionInFlight_ = false;
            self->badges_.selectedChannelBadge = badge;
            self->channel_->addSystemMessage(
                QStringLiteral("Channel badge set to: %1").arg(badge.title));
            self->rebuildContent();
        },
        [self](const QString &error) {
            if (!self)
                return;
            self->actionInFlight_ = false;
            self->setStatus(
                MoltorinoAuth::normalizeAuthError("selecting badge", error),
                true);
            self->rebuildContent();
        });
}

void TwitchBadgePickerDialog::refreshStyle()
{
    auto *fonts = getApp()->getFonts();
    const auto scale = this->scale();
    this->headerTitleLabel_->setFont(
        fonts->getFont(FontStyle::UiMediumBold, scale * 1.2F));
    this->globalTabButton_->setFont(fonts->getFont(FontStyle::UiMedium, scale));
    this->channelTabButton_->setFont(fonts->getFont(FontStyle::UiMedium, scale));

    const auto *theme = this->theme;
    const auto bg = theme->window.background.name();
    const auto text = theme->window.text.name(QColor::HexArgb);
    const auto border = theme->splits.header.border.name();
    const auto buttonBg = theme->splits.input.background.name();
    const auto focusedBorder = theme->splits.header.focusedBorder.name();
    const auto hoverBg =
        theme->isLightTheme()
            ? theme->splits.input.background.darker(104).name()
            : theme->splits.input.background.lighter(108).name();

    this->setStyleSheet(QStringLiteral(R"(
        QWidget#TwitchBadgePickerDialog { background: %1; color: %2; }
        QLabel#TwitchBadgePickerTitle { color: %2; font-weight: 700; }
        QLabel#TwitchBadgePickerSectionLabel { color: %2; font-weight: 600; padding-top: 4px; }
        QLabel#TwitchBadgePickerStatus { color: %2; }
        QScrollArea { background: transparent; border: 0; }
        QScrollBar:vertical { width: 6px; }
        QPushButton#TwitchBadgePickerTab {
            background: %4; color: %2;
            border: 1px solid %3; border-radius: 4px; padding: 3px 10px;
        }
        QPushButton#TwitchBadgePickerTab:checked {
            background: #9146ff; color: #ffffff; border-color: #9146ff;
        }
        QPushButton#TwitchBadgePickerTab:hover:!checked {
            background: %6; border-color: %5;
        }
        QPushButton#TwitchBadgePickerNoBadge {
            background: %4; color: %2;
            border: 1px solid %3; border-radius: 4px; padding: 4px 8px;
        }
        QPushButton#TwitchBadgePickerNoBadge:hover { background: %6; border-color: %5; }
    )")
                            .arg(bg, text, border, buttonBg, focusedBorder,
                                 hoverBg));
}

void TwitchBadgePickerDialog::applySizeConstraints()
{
    const int w = std::max(1, int(DEFAULT_DIALOG_SIZE.width() * this->scale()));
    const int h = std::max(1, int(DEFAULT_DIALOG_SIZE.height() * this->scale()));
    this->setMinimumSize(
        QSize(std::min(w, std::max(180, int(220 * this->scale()))),
              std::min(h, std::max(120, int(180 * this->scale())))));
}

QString TwitchBadgePickerDialog::authTokenOrMessage()
{
    QString authError;
    const auto auth = MoltorinoAuth::resolveCurrentUserToken(&authError);
    if (auth.hasToken())
        return auth.token;

    const auto message =
        authError.isEmpty()
            ? MoltorinoAuth::authRequiredMessage("using badge picker")
            : authError;
    this->setStatus(message, true);
    return {};
}

}  // namespace chatterino

#endif
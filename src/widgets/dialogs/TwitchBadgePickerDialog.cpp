// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/TwitchBadgePickerDialog.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Image.hpp"
#include "messages/ImageSet.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Twitch.hpp"
#include "widgets/buttons/Button.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/dialogs/ColorPickerDialog.hpp"
#include "widgets/helper/color/ColorButton.hpp"
#include "widgets/helper/Line.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QColor>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QShowEvent>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>

namespace chatterino {

namespace {

constexpr QSize DEFAULT_DIALOG_SIZE(332, 400);
constexpr int HEADER_SEPARATOR_HEIGHT = 8;
constexpr int BADGE_GRID_COLUMNS = 6;
constexpr int BADGE_TILE_PADDING = 4;
constexpr QSize BADGE_ICON_SIZE(36, 36);
constexpr float BADGE_IMAGE_SCALE =
    float(BADGE_ICON_SIZE.width()) / float(BADGE_ICON_SIZE.width() / 2);
constexpr char BADGE_SELECTED_COLOR[] = "#9146ff";

int scaledSeparatorHeight(float scale)
{
    return std::max(1, int(HEADER_SEPARATOR_HEIGHT * scale));
}

int scaledMetric(float scale, int base, int minimum)
{
    return std::max(minimum, int(std::round(base * scale)));
}

int contentHorizontalMargin(float scale)
{
    return scaledMetric(scale, 12, 6);
}

bool badgeMatchesSearch(const GqlBadge &badge, const QString &needle)
{
    if (needle.isEmpty())
    {
        return true;
    }

    return badge.title.contains(needle, Qt::CaseInsensitive) ||
           badge.setID.contains(needle, Qt::CaseInsensitive);
}

QLabel *makeEmptyListLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName("TwitchBadgePickerEmpty");
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    return label;
}

ImageSet badgeImages(const GqlBadge &badge)
{
    const auto base = BADGE_ICON_SIZE / 2;

    ImagePtr image1;
    ImagePtr image2;
    ImagePtr image3;

    if (!badge.image1x.isEmpty())
    {
        image1 = Image::fromUrl(Url{badge.image1x}, 2, base);
    }
    if (!badge.image2x.isEmpty())
    {
        image2 = Image::fromUrl(Url{badge.image2x}, 1, BADGE_ICON_SIZE);
    }
    if (!badge.image4x.isEmpty())
    {
        image3 = Image::fromUrl(Url{badge.image4x}, 0.5, BADGE_ICON_SIZE * 2);
    }

    // Fall back to the best available resolution when a slot is missing.
    if (!image2 && image1)
    {
        image2 = image1;
    }
    if (!image3 && image2)
    {
        image3 = image2;
    }
    if (!image1 && image2)
    {
        image1 = image2;
    }
    if (!image2 && image3)
    {
        image2 = image3;
    }
    if (!image1 && image3)
    {
        image1 = image3;
    }

    const auto empty = getEmptyImagePtr();
    return ImageSet{
        image1 ? image1 : empty,
        image2 ? image2 : empty,
        image3 ? image3 : empty,
    };
}

void paintBadgeTileBackground(QPainter &painter, const QRect &rect,
                              bool underMouse, bool isDown)
{
    const auto *theme = getApp()->getThemes();
    auto bg = theme->splits.header.background;
    auto border = theme->splits.header.border;

    if (underMouse)
    {
        bg = theme->isLightTheme() ? bg.darker(105) : bg.lighter(115);
        border = theme->splits.header.focusedBorder;
    }
    if (isDown)
    {
        bg = theme->isLightTheme() ? bg.darker(112) : bg.lighter(125);
    }

    painter.setPen(QPen(border, 1));
    painter.setBrush(bg);
    painter.drawRoundedRect(rect, 3, 3);
}

void paintBadgeTileFocusRing(QPainter &painter, const QRect &rect)
{
    auto focus = getApp()->getThemes()->window.text;
    focus.setAlpha(200);
    painter.setPen(QPen(focus, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect.adjusted(1, 1, -1, -1), 3, 3);
}

class BadgePickerToggleSwitch final : public QPushButton
{
public:
    BadgePickerToggleSwitch(QWidget *parent)
        : QPushButton(parent)
    {
        this->setObjectName("TwitchBadgePickerToggleSwitch");
        this->setCheckable(true);
        this->setFlat(true);
        this->setCursor(Qt::PointingHandCursor);
        this->setFocusPolicy(Qt::StrongFocus);
        this->setAttribute(Qt::WA_Hover, true);
        this->updateMetrics(1.0F);
    }

    void updateMetrics(float scale)
    {
        this->setFixedSize(scaledMetric(scale, 38, 28),
                           scaledMetric(scale, 20, 14));
        this->update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const auto *theme = getApp()->getThemes();
        const auto track = this->rect().adjusted(0, 0, -1, -1);
        const int knobMargin = 2;
        const int knobDiameter = track.height() - knobMargin * 2;

        auto trackBg = theme->splits.input.background;
        auto trackBorder = theme->splits.header.border;

        if (this->isChecked())
        {
            trackBg = QColor("#9146ff");
            trackBorder = QColor("#9146ff");
        }
        else if (this->underMouse() && this->isEnabled())
        {
            trackBg = theme->isLightTheme() ? trackBg.darker(104)
                                            : trackBg.lighter(108);
            trackBorder = theme->splits.header.focusedBorder;
        }

        if (!this->isEnabled())
        {
            trackBg.setAlpha(120);
            trackBorder.setAlpha(120);
        }

        const qreal radius = track.height() / 2.0;
        painter.setPen(QPen(trackBorder, 1));
        painter.setBrush(trackBg);
        painter.drawRoundedRect(track, radius, radius);

        const int knobX = this->isChecked()
                              ? track.right() - knobMargin - knobDiameter + 1
                              : track.left() + knobMargin;
        const QRectF knobRect(knobX, track.top() + knobMargin, knobDiameter,
                              knobDiameter);

        auto knobColor = QColor("#ffffff");
        if (!this->isEnabled())
        {
            knobColor.setAlpha(180);
        }

        painter.setPen(Qt::NoPen);
        painter.setBrush(knobColor);
        painter.drawEllipse(knobRect);

        if (this->hasFocus())
        {
            paintBadgeTileFocusRing(painter, track);
        }
    }
};

QWidget *makeSettingRow(const QString &labelText,
                        BadgePickerToggleSwitch *toggle, QWidget *parent,
                        float scale)
{
    auto *row = new QWidget(parent);
    row->setObjectName("TwitchBadgePickerSettingRow");

    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(
        scaledMetric(scale, 8, 4), scaledMetric(scale, 6, 3),
        scaledMetric(scale, 8, 4), scaledMetric(scale, 6, 3));
    rowLayout->setSpacing(scaledMetric(scale, 8, 4));

    auto *label = new QLabel(labelText, row);
    label->setObjectName("TwitchBadgePickerSettingLabel");
    label->setWordWrap(true);
    label->setFont(getApp()->getFonts()->getFont(FontStyle::UiMedium, scale));

    rowLayout->addWidget(label, 1);
    rowLayout->addWidget(toggle, 0, Qt::AlignRight | Qt::AlignVCenter);

    return row;
}

class NoBadgeTileButton final : public QPushButton
{
public:
    NoBadgeTileButton(QWidget *parent)
        : QPushButton(parent)
    {
        this->setCursor(Qt::PointingHandCursor);
        this->setFocusPolicy(Qt::StrongFocus);
        this->setAttribute(Qt::WA_Hover, true);
        this->setFlat(true);
        this->setFixedSize(BADGE_ICON_SIZE.width() + BADGE_TILE_PADDING * 2,
                           BADGE_ICON_SIZE.height() + BADGE_TILE_PADDING * 2);
        this->setToolTip("No Badge");
    }

    void setSelected(bool selected)
    {
        this->selected_ = selected;
        this->update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const auto rect = this->rect().adjusted(0, 0, -1, -1);

        if (this->selected_)
        {
            const auto selectionRect = this->rect().adjusted(1, 1, -2, -2);
            painter.setPen(QPen(QColor(BADGE_SELECTED_COLOR), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(selectionRect, 3, 3);
            const auto innerRect = selectionRect.adjusted(2, 2, -2, -2);
            paintBadgeTileBackground(painter, innerRect, this->underMouse(),
                                     this->isDown());
        }
        else
        {
            paintBadgeTileBackground(painter, rect, this->underMouse(),
                                     this->isDown());
        }

        const int pad = BADGE_TILE_PADDING;
        const QRect iconRect(pad, pad, rect.width() - pad * 2,
                             rect.height() - pad * 2);
        const auto center = iconRect.center();
        const int radius =
            std::min(iconRect.width(), iconRect.height()) / 2 - 3;

        auto iconColor = getApp()->getThemes()->window.text;
        iconColor.setAlpha(220);
        painter.setPen(QPen(iconColor, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(center, radius, radius);

        const qreal diagonal = radius * 0.70710678118;
        painter.drawLine(QPointF(center.x() - diagonal, center.y() - diagonal),
                         QPointF(center.x() + diagonal, center.y() + diagonal));

        if (this->hasFocus())
        {
            paintBadgeTileFocusRing(painter, rect);
        }
    }

private:
    bool selected_ = false;
};

class BadgeTileButton final : public QPushButton
{
public:
    BadgeTileButton(const GqlBadge &badge, QWidget *parent)
        : QPushButton(parent)
        , badge_(badge)
        , images_(badgeImages(badge))
    {
        this->setCursor(Qt::PointingHandCursor);
        this->setFocusPolicy(Qt::StrongFocus);
        this->setAttribute(Qt::WA_Hover, true);
        this->setFlat(true);
        this->setFixedSize(BADGE_ICON_SIZE.width() + BADGE_TILE_PADDING * 2,
                           BADGE_ICON_SIZE.height() + BADGE_TILE_PADDING * 2);
        this->setToolTip(badge.title);

        this->connections_.managedConnect(
            getApp()->getWindows()->layoutRequested, [this](Channel *) {
                this->refreshImageIfNeeded();
            });
    }

    const GqlBadge &badge() const
    {
        return this->badge_;
    }

    void setSelected(bool selected)
    {
        this->selected_ = selected;
        this->update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

        const auto rect = this->rect().adjusted(0, 0, -1, -1);

        if (this->selected_)
        {
            const auto selectionRect = this->rect().adjusted(1, 1, -2, -2);
            painter.setPen(QPen(QColor(BADGE_SELECTED_COLOR), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(selectionRect, 3, 3);
            const auto innerRect = selectionRect.adjusted(2, 2, -2, -2);
            paintBadgeTileBackground(painter, innerRect, this->underMouse(),
                                     this->isDown());
        }
        else
        {
            paintBadgeTileBackground(painter, rect, this->underMouse(),
                                     this->isDown());
        }

        const int pad = BADGE_TILE_PADDING;
        const QRect imgRect(pad, pad, rect.width() - pad * 2,
                            rect.height() - pad * 2);

        const auto &image = this->images_.getImageOrLoaded(BADGE_IMAGE_SCALE);
        if (auto pixmap = image->pixmapOrLoad())
        {
            this->attemptRefresh_ = false;
            painter.drawPixmap(imgRect, *pixmap, pixmap->rect());
        }
        else
        {
            this->attemptRefresh_ = true;
            auto muted = getApp()->getThemes()->window.text;
            muted.setAlpha(50);
            painter.setBrush(muted);
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(imgRect, 2, 2);
        }

        if (this->hasFocus())
        {
            paintBadgeTileFocusRing(painter, rect);
        }
    }

private:
    void refreshImageIfNeeded()
    {
        if (!this->attemptRefresh_)
        {
            return;
        }

        const auto &image = this->images_.getImageOrLoaded(BADGE_IMAGE_SCALE);
        if (image->pixmapOrLoad())
        {
            this->attemptRefresh_ = false;
            this->update();
        }
    }

    GqlBadge badge_;
    ImageSet images_;
    bool selected_ = false;
    bool attemptRefresh_ = false;
    pajlada::Signals::SignalHolder connections_;
};

class ColorTileButton final : public QPushButton
{
public:
    ColorTileButton(const QColor &color, const QString &label, QWidget *parent)
        : QPushButton(parent)
        , color_(color)
        , label_(label)
    {
        this->setCursor(Qt::PointingHandCursor);
        this->setFocusPolicy(Qt::StrongFocus);
        this->setAttribute(Qt::WA_Hover, true);
        this->setFlat(true);
        this->setFixedSize(BADGE_ICON_SIZE.width() + BADGE_TILE_PADDING * 2,
                           BADGE_ICON_SIZE.height() + BADGE_TILE_PADDING * 2);
        this->setToolTip(label);
    }

    void setSelected(bool selected)
    {
        this->selected_ = selected;
        this->update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const auto rect = this->rect().adjusted(0, 0, -1, -1);

        if (this->selected_)
        {
            const auto selectionRect = this->rect().adjusted(1, 1, -2, -2);
            painter.setPen(QPen(QColor(BADGE_SELECTED_COLOR), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(selectionRect, 3, 3);
            const auto innerRect = selectionRect.adjusted(2, 2, -2, -2);
            paintBadgeTileBackground(painter, innerRect, this->underMouse(),
                                     this->isDown());
        }
        else
        {
            paintBadgeTileBackground(painter, rect, this->underMouse(),
                                     this->isDown());
        }

        const int pad = BADGE_TILE_PADDING;
        const QRect colorRect(pad, pad, rect.width() - pad * 2,
                              rect.height() - pad * 2);

        painter.setPen(QPen(getApp()->getThemes()->splits.header.border, 1));
        painter.setBrush(this->color_);
        painter.drawRoundedRect(colorRect, 2, 2);

        if (this->hasFocus())
        {
            paintBadgeTileFocusRing(painter, rect);
        }
    }

private:
    QColor color_;
    QString label_;
    bool selected_ = false;
};

bool isValidCustomHexColor(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    return re.match(text.trimmed()).hasMatch();
}

QString normalizeCustomHexInput(const QString &text)
{
    const auto trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith('#'))
    {
        return trimmed;
    }

    static const QRegularExpression hexOnly(
        QStringLiteral("^[0-9A-Fa-f]{1,6}$"));
    if (hexOnly.match(trimmed).hasMatch())
    {
        return '#' + trimmed;
    }

    return trimmed;
}

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
    container->setObjectName("TwitchBadgePickerDialogRoot");
    container->setMouseTracking(true);
    this->mainLayout_ = new QVBoxLayout(container);
    this->mainLayout_->setSpacing(0);

    // Header
    this->headerWidget_ = new QWidget(container);
    this->headerWidget_->setObjectName("TwitchBadgePickerHeader");
    auto *headerLayout = new QHBoxLayout(this->headerWidget_);
    headerLayout->setContentsMargins(0, 0, 0, 0);
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
    this->closeButton_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    QObject::connect(this->closeButton_, &Button::leftClicked, this,
                     &QWidget::close);
    headerLayout->addWidget(this->closeButton_);
    this->mainLayout_->addWidget(this->headerWidget_);

    auto *separator = new Line(false);
    separator->setObjectName("TwitchBadgePickerDialogSeparator");
    separator->setFixedHeight(scaledSeparatorHeight(this->scale()));
    this->mainLayout_->addWidget(separator);

    // Tab rows
    const int tabSpacing = 4;
    const int tabTopMargin = scaledMetric(this->scale(), 4, 2);
    auto *tabRows = new QVBoxLayout();
    tabRows->setSpacing(tabSpacing);
    tabRows->setContentsMargins(0, tabTopMargin, 0, 0);

    auto *badgeTabRow = new QHBoxLayout();
    badgeTabRow->setSpacing(tabSpacing);

    this->globalTabButton_ = new QPushButton("Global Badge", container);
    this->globalTabButton_->setObjectName("TwitchBadgePickerTab");
    this->globalTabButton_->setCheckable(true);
    this->globalTabButton_->setChecked(true);
    this->globalTabButton_->setCursor(Qt::PointingHandCursor);
    this->globalTabButton_->setSizePolicy(QSizePolicy::Expanding,
                                          QSizePolicy::Fixed);
    QObject::connect(this->globalTabButton_, &QPushButton::clicked, this,
                     [this] {
                         this->switchView(View::GlobalBadges);
                     });

    this->channelTabButton_ = new QPushButton("Channel Badge", container);
    this->channelTabButton_->setObjectName("TwitchBadgePickerTab");
    this->channelTabButton_->setCheckable(true);
    this->channelTabButton_->setChecked(false);
    this->channelTabButton_->setCursor(Qt::PointingHandCursor);
    this->channelTabButton_->setSizePolicy(QSizePolicy::Expanding,
                                           QSizePolicy::Fixed);
    QObject::connect(this->channelTabButton_, &QPushButton::clicked, this,
                     [this] {
                         this->switchView(View::ChannelBadges);
                     });

    badgeTabRow->addWidget(this->globalTabButton_, 1);
    badgeTabRow->addWidget(this->channelTabButton_, 1);
    tabRows->addLayout(badgeTabRow);

    auto *otherTabRow = new QHBoxLayout();
    otherTabRow->setSpacing(tabSpacing);

    this->colorTabButton_ = new QPushButton("Color", container);
    this->colorTabButton_->setObjectName("TwitchBadgePickerTab");
    this->colorTabButton_->setCheckable(true);
    this->colorTabButton_->setChecked(false);
    this->colorTabButton_->setCursor(Qt::PointingHandCursor);
    this->colorTabButton_->setSizePolicy(QSizePolicy::Expanding,
                                         QSizePolicy::Fixed);
    QObject::connect(this->colorTabButton_, &QPushButton::clicked, this,
                     [this] {
                         this->switchView(View::Color);
                     });

    this->eventTabButton_ = new QPushButton("Missing / Soon", container);
    this->eventTabButton_->setObjectName("TwitchBadgePickerTab");
    this->eventTabButton_->setCheckable(true);
    this->eventTabButton_->setChecked(false);
    this->eventTabButton_->setCursor(Qt::PointingHandCursor);
    this->eventTabButton_->setSizePolicy(QSizePolicy::Expanding,
                                         QSizePolicy::Fixed);
    QObject::connect(this->eventTabButton_, &QPushButton::clicked, this,
                     [this] {
                         this->switchView(View::EventBadges);
                     });

    otherTabRow->addWidget(this->colorTabButton_, 1);
    otherTabRow->addWidget(this->eventTabButton_, 1);
    tabRows->addLayout(otherTabRow);
    this->mainLayout_->addLayout(tabRows);

    this->searchInput_ = new QLineEdit(container);
    this->searchInput_->setObjectName("TwitchBadgePickerSearch");
    this->searchInput_->setPlaceholderText("Search...");
    this->searchInput_->setClearButtonEnabled(true);
    QObject::connect(this->searchInput_, &QLineEdit::textChanged, this,
                     [this](const QString &text) {
                         this->searchQuery_ = text;
                         this->rebuildContent();
                         if (this->searchInput_ != nullptr)
                         {
                             this->searchInput_->setFocus(Qt::OtherFocusReason);
                             this->searchInput_->setCursorPosition(
                                 this->searchQuery_.size());
                         }
                     });
    this->searchRowWidget_ = new QWidget(container);
    auto *searchRow = new QHBoxLayout(this->searchRowWidget_);
    searchRow->setContentsMargins(0, scaledMetric(this->scale(), 4, 2), 0,
                                  scaledMetric(this->scale(), 4, 2));
    searchRow->addWidget(this->searchInput_);
    this->mainLayout_->addWidget(this->searchRowWidget_);

    // Scroll area
    this->scrollArea_ = new QScrollArea(container);
    this->scrollArea_->setObjectName("TwitchBadgePickerScrollArea");
    this->scrollArea_->setFrameShape(QFrame::NoFrame);
    this->scrollArea_->setWidgetResizable(true);
    this->scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->scrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    this->scrollArea_->viewport()->setAutoFillBackground(false);
    this->mainLayout_->addWidget(this->scrollArea_, 1);

    this->contentWidget_ = new QWidget();
    this->contentWidget_->setObjectName("TwitchBadgePickerDialogContent");
    this->contentWidget_->setMinimumWidth(0);
    this->contentWidget_->setSizePolicy(QSizePolicy::Ignored,
                                        QSizePolicy::Preferred);
    this->contentLayout_ = new QVBoxLayout(this->contentWidget_);
    this->contentLayout_->setContentsMargins(
        0, scaledMetric(this->scale(), 7, 4), 0,
        scaledMetric(this->scale(), 8, 4));
    this->contentLayout_->setSpacing(scaledMetric(this->scale(), 7, 4));
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
    dialog->showAndMoveTo(center - QPoint(size.width() / 2, size.height() / 2),
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

    if (this->scrollArea_ != nullptr)
    {
        this->scrollArea_->horizontalScrollBar()->setValue(0);
    }
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

    if (this->badgesLoading_ && this->view_ != View::Color)
    {
        this->setStatus("Loading badges...");
        this->contentLayout_->addStretch(1);
        return;
    }

    if (this->view_ == View::GlobalBadges)
        this->rebuildGlobalBadges();
    else if (this->view_ == View::ChannelBadges)
        this->rebuildChannelBadges();
    else if (this->view_ == View::EventBadges)
        this->rebuildEventBadges();
    else
        this->rebuildColors();

    this->contentLayout_->addStretch(1);
    this->applySizeConstraints();

    if (this->scrollArea_ != nullptr)
    {
        this->scrollArea_->horizontalScrollBar()->setValue(0);
    }
}

void TwitchBadgePickerDialog::rebuildGlobalBadges()
{
    const auto &available = this->badges_.availableGlobal;
    const auto &selected = this->badges_.selectedGlobalBadge;
    const auto needle = this->searchQuery_.trimmed();

    if (available.isEmpty())
    {
        if (!(this->statusIsError_ && !this->statusText_.isEmpty()))
            this->setStatus("No global badges available.");
        return;
    }

    auto *label = new QLabel("Your Badges", this->contentWidget_);
    label->setObjectName("TwitchBadgePickerSectionLabel");
    this->contentLayout_->addWidget(label);

    auto *gridWidget = new QWidget(this->contentWidget_);
    auto *grid = new QGridLayout(gridWidget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(4);

    int row = 0;
    int col = 0;

    if (needle.isEmpty())
    {
        auto *noBadgeTile = new NoBadgeTileButton(gridWidget);
        noBadgeTile->setEnabled(!this->actionInFlight_);
        if (selected.setID.isEmpty())
            noBadgeTile->setSelected(true);
        QObject::connect(noBadgeTile, &QPushButton::clicked, this, [this] {
            this->selectGlobal({});
        });
        grid->addWidget(noBadgeTile, row, col);
        col = 1;
    }

    int shown = 0;
    for (const auto &badge : available)
    {
        if (!badgeMatchesSearch(badge, needle))
        {
            continue;
        }

        auto *tile = new BadgeTileButton(badge, gridWidget);
        tile->setEnabled(!this->actionInFlight_);

        if (!selected.setID.isEmpty() && badge.setID == selected.setID &&
            badge.version == selected.version)
            tile->setSelected(true);

        QObject::connect(tile, &QPushButton::clicked, this, [this, badge] {
            this->selectGlobal(badge);
        });
        grid->addWidget(tile, row, col);
        ++shown;
        if (++col >= BADGE_GRID_COLUMNS)
        {
            col = 0;
            ++row;
        }
    }

    if (shown == 0 && !needle.isEmpty())
    {
        if (!this->statusIsError_)
        {
            this->setStatus({});
        }
        this->contentLayout_->addWidget(makeEmptyListLabel(
            QStringLiteral("No matching badges."), this->contentWidget_));
        return;
    }

    if (!this->statusIsError_)
    {
        this->setStatus({});
    }

    this->contentLayout_->addWidget(gridWidget);
}

void TwitchBadgePickerDialog::rebuildChannelBadges()
{
    const auto &available = this->badges_.availableChannel;
    const auto &selected = this->badges_.selectedChannelBadge;
    const auto needle = this->searchQuery_.trimmed();
    const auto scale = this->scale();

    auto *toggleButton = new BadgePickerToggleSwitch(this->contentWidget_);
    toggleButton->setChecked(this->badges_.useCustomChannelBadge);
    toggleButton->setEnabled(!this->actionInFlight_);
    toggleButton->updateMetrics(scale);

    QObject::connect(
        toggleButton, &QPushButton::clicked, this, [this](bool checked) {
            if (checked)
            {
                if (!this->badges_.selectedChannelBadge.setID.isEmpty())
                {
                    this->selectChannel(this->badges_.selectedChannelBadge);
                }
                else
                {
                    this->badges_.useCustomChannelBadge = true;
                    this->rebuildContent();
                }
            }
            else
            {
                this->deselectChannel();
            }
        });

    this->contentLayout_->addWidget(
        makeSettingRow("Use Custom Badge for This Channel", toggleButton,
                       this->contentWidget_, scale));

    if (this->badges_.subscriptionTier >= 2000)
    {
        auto *flairButton = new BadgePickerToggleSwitch(this->contentWidget_);
        flairButton->setChecked(!this->badges_.isBadgeModifierHidden);
        flairButton->setEnabled(!this->actionInFlight_);
        flairButton->updateMetrics(scale);

        QObject::connect(flairButton, &QPushButton::clicked, this,
                         [this](bool checked) {
                             this->setFlairHidden(!checked);
                         });

        this->contentLayout_->addWidget(
            makeSettingRow("Badge Flair for Tier 2 and 3 Subscriptions",
                           flairButton, this->contentWidget_, scale));
    }

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
    int shown = 0;
    for (const auto &badge : available)
    {
        if (!badgeMatchesSearch(badge, needle))
        {
            continue;
        }

        auto *tile = new BadgeTileButton(badge, gridWidget);
        tile->setEnabled(!this->actionInFlight_ &&
                         this->badges_.useCustomChannelBadge);

        if (!selected.setID.isEmpty() && badge.setID == selected.setID &&
            badge.version == selected.version &&
            this->badges_.useCustomChannelBadge)
            tile->setSelected(true);

        QObject::connect(tile, &QPushButton::clicked, this, [this, badge] {
            this->selectChannel(badge);
        });
        grid->addWidget(tile, row, col);
        ++shown;
        if (++col >= BADGE_GRID_COLUMNS)
        {
            col = 0;
            ++row;
        }
    }

    if (shown == 0)
    {
        if (needle.isEmpty())
        {
            this->setStatus("No channel-specific badges available.");
        }
        else
        {
            if (!this->statusIsError_)
            {
                this->setStatus({});
            }
            this->contentLayout_->addWidget(makeEmptyListLabel(
                QStringLiteral("No matching badges."), this->contentWidget_));
        }
        return;
    }

    if (!this->statusIsError_)
    {
        this->setStatus({});
    }

    this->contentLayout_->addWidget(gridWidget);
}

void TwitchBadgePickerDialog::loadEventBadges(bool force)
{
    if (this->eventBadgesLoading_)
        return;

    // cache de 10 minutos
    if (!force && this->eventBadgesLoaded_ &&
        this->eventBadgesCacheTime_.secsTo(QDateTime::currentDateTimeUtc()) <
            600)
        return;

    this->eventBadgesLoading_ = true;
    this->rebuildContent();

    QPointer<TwitchBadgePickerDialog> self = this;

    NetworkRequest("https://api.catquery.com/eventBadges")
        .onSuccess([self](const NetworkResult &result) {
            if (!self)
                return;
            self->eventBadgesLoading_ = false;
            self->eventBadgesLoaded_ = true;
            self->eventBadgesCacheTime_ = QDateTime::currentDateTimeUtc();
            self->eventBadgesCache_.clear();

            const auto arr =
                result.parseJsonValue().toObject().value("badges").toArray();
            for (const auto &val : arr)
            {
                const auto obj = val.toObject();
                EventBadge b;
                b.id = obj.value("id").toString();
                b.name = obj.value("name").toString();
                b.imageUrl = obj.value("imageUrl").toString();
                b.streamDatabaseUrl = obj.value("streamdatabaseUrl").toString();
                b.startAt = QDateTime::fromString(
                    obj.value("startAt").toString(), Qt::ISODate);
                b.endAt = QDateTime::fromString(obj.value("endAt").toString(),
                                                Qt::ISODate);
                if (!obj.value("free").isNull())
                    b.free = obj.value("free").toBool();
                self->eventBadgesCache_.push_back(b);
            }
            self->setStatus({});
            self->rebuildContent();
        })
        .onError([self](const NetworkResult &) {
            if (!self)
                return;
            self->eventBadgesLoading_ = false;
            self->setStatus("Failed to load event badges.", true);
            self->rebuildContent();
        })
        .execute();
}

void TwitchBadgePickerDialog::rebuildEventBadges()
{
    if (this->eventBadgesLoading_)
    {
        this->setStatus("Loading event badges...");
        return;
    }

    if (!this->eventBadgesLoaded_)
    {
        this->setStatus("No data loaded.");
        return;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    const auto needle = this->searchQuery_.trimmed();

    // set de IDs que o usuário já tem
    QSet<QString> ownedIds;
    for (const auto &b : this->badges_.availableGlobal)
        ownedIds.insert(b.setID);

    QVector<EventBadge> missing, comingSoon;
    for (const auto &b : this->eventBadgesCache_)
    {
        if (!needle.isEmpty() &&
            !b.name.contains(needle, Qt::CaseInsensitive) &&
            !b.id.contains(needle, Qt::CaseInsensitive))
            continue;

        if (b.startAt > now)
            comingSoon.push_back(b);
        else if (b.endAt > now && !ownedIds.contains(b.id))
            missing.push_back(b);
    }

    auto makeTile = [&](const EventBadge &badge,
                        QWidget *parent) -> BadgeTileButton * {
        GqlBadge gql;
        gql.setID = badge.id;
        gql.title = badge.name;
        gql.image2x = badge.imageUrl;
        auto *tile = new BadgeTileButton(gql, parent);

        // tooltip com info da badge
        QString tip = badge.name;
        if (badge.endAt.isValid())
            tip += QStringLiteral("\nEnds: %1")
                       .arg(badge.endAt.toLocalTime().toString(
                           "MMM d, yyyy hh:mm"));
        if (badge.startAt.isValid() && badge.startAt > now)
            tip += QStringLiteral("\nStarts: %1")
                       .arg(badge.startAt.toLocalTime().toString(
                           "MMM d, yyyy hh:mm"));
        if (badge.free.has_value())
            tip += badge.free.value() ? "\nFree" : "\nPaid";
        tile->setToolTip(tip);

        if (!badge.streamDatabaseUrl.isEmpty())
        {
            const auto url = badge.streamDatabaseUrl;
            QObject::connect(tile, &QPushButton::clicked, this, [url] {
                QDesktopServices::openUrl(QUrl(url));
            });
        }
        return tile;
    };

    auto makeGrid = [&](const QVector<EventBadge> &badges) {
        auto *gridWidget = new QWidget(this->contentWidget_);
        auto *grid = new QGridLayout(gridWidget);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(4);
        int row = 0;
        int col = 0;
        for (const auto &badge : badges)
        {
            auto *tile = makeTile(badge, gridWidget);
            grid->addWidget(tile, row, col);
            if (++col >= BADGE_GRID_COLUMNS)
            {
                col = 0;
                ++row;
            }
        }
        this->contentLayout_->addWidget(gridWidget);
    };

    if (!missing.isEmpty())
    {
        auto *label =
            new QLabel("Missing (Available Now)", this->contentWidget_);
        label->setObjectName("TwitchBadgePickerSectionLabel");
        this->contentLayout_->addWidget(label);
        makeGrid(missing);
    }

    if (!comingSoon.isEmpty())
    {
        auto *label = new QLabel("Coming Soon", this->contentWidget_);
        label->setObjectName("TwitchBadgePickerSectionLabel");
        this->contentLayout_->addWidget(label);
        makeGrid(comingSoon);
    }

    if (missing.isEmpty() && comingSoon.isEmpty())
    {
        if (needle.isEmpty())
            this->setStatus("You already have all available event badges!");
        else
            this->setStatus("No event badges match your search.");
    }
    else if (!this->statusIsError_)
    {
        this->setStatus({});
    }
}

void TwitchBadgePickerDialog::switchView(View view)
{
    this->view_ = view;
    this->globalTabButton_->setChecked(view == View::GlobalBadges);
    this->channelTabButton_->setChecked(view == View::ChannelBadges);
    this->eventTabButton_->setChecked(view == View::EventBadges);
    this->colorTabButton_->setChecked(view == View::Color);
    this->updateSearchVisibility();

    if (view == View::EventBadges)
    {
        this->loadEventBadges(false);
    }

    this->rebuildContent();
}

void TwitchBadgePickerDialog::updateSearchVisibility()
{
    if (this->searchRowWidget_ != nullptr)
    {
        this->searchRowWidget_->setVisible(this->view_ != View::Color);
    }
}

void TwitchBadgePickerDialog::rebuildColors()
{
    const auto user = getApp()->getAccounts()->twitch.getCurrent();
    if (user->isAnon())
    {
        this->setStatus("You must be logged in to change your color.", true);
        return;
    }

    const auto currentColor = user->color();
    const auto currentHex = currentColor.isValid()
                                ? currentColor.name(QColor::HexRgb).toUpper()
                                : QString{};
    const auto selectedPreset = helixColorNameFromDisplayHex(currentHex);

    auto *label = new QLabel("Twitch Colors", this->contentWidget_);
    label->setObjectName("TwitchBadgePickerSectionLabel");
    this->contentLayout_->addWidget(label);

    auto *gridWidget = new QWidget(this->contentWidget_);
    auto *grid = new QGridLayout(gridWidget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(4);

    int row = 0;
    int col = 0;
    for (const auto &name : VALID_HELIX_COLORS)
    {
        const auto hex = helixColorDisplayHex(name);
        if (hex.isEmpty())
        {
            continue;
        }

        auto *tile = new ColorTileButton(
            QColor(hex), formatHelixColorLabel(name), gridWidget);
        tile->setEnabled(!this->actionInFlight_);
        if (selectedPreset && *selectedPreset == name)
        {
            tile->setSelected(true);
        }

        QObject::connect(tile, &QPushButton::clicked, this, [this, name] {
            this->selectColor(name);
        });

        grid->addWidget(tile, row, col);
        if (++col >= BADGE_GRID_COLUMNS)
        {
            col = 0;
            ++row;
        }
    }
    this->contentLayout_->addWidget(gridWidget);

    auto *customLabel =
        new QLabel("Custom (Turbo / Prime)", this->contentWidget_);
    customLabel->setObjectName("TwitchBadgePickerSectionLabel");
    this->contentLayout_->addWidget(customLabel);

    auto *customRow = new QWidget(this->contentWidget_);
    customRow->setObjectName("TwitchBadgePickerCustomColorRow");
    auto *customLayout = new QHBoxLayout(customRow);
    customLayout->setContentsMargins(0, 0, 0, 0);
    customLayout->setSpacing(scaledMetric(this->scale(), 6, 4));

    const int controlHeight = scaledMetric(this->scale(), 28, 22);
    const auto scale = this->scale();

    auto *hexInput = new QLineEdit(customRow);
    hexInput->setObjectName("TwitchBadgePickerHexInput");
    hexInput->setPlaceholderText("#RRGGBB");
    hexInput->setMaxLength(7);
    hexInput->setFixedHeight(controlHeight);
    hexInput->setFont(
        getApp()->getFonts()->getFont(FontStyle::ChatMediumMono, scale));
    hexInput->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("^#?[0-9A-Fa-f]{0,6}$")), hexInput));
    if (!selectedPreset && !currentHex.isEmpty())
    {
        hexInput->setText(currentHex);
    }

    QColor swatchColor(QStringLiteral("#808080"));
    if (!selectedPreset && !currentHex.isEmpty())
    {
        swatchColor = QColor(currentHex);
    }

    auto *colorButton = new ColorButton(swatchColor, customRow);
    colorButton->setFixedSize(controlHeight, controlHeight);
    colorButton->setCursor(Qt::PointingHandCursor);
    colorButton->setToolTip("Open color picker");
    colorButton->setEnabled(!this->actionInFlight_);

    auto updatePreview = [colorButton, hexInput] {
        const auto text = normalizeCustomHexInput(hexInput->text());
        if (isValidCustomHexColor(text))
        {
            colorButton->setColor(QColor(text));
        }
        else
        {
            colorButton->setColor(QColor(0, 0, 0, 0));
        }
    };
    updatePreview();

    auto *applyButton = new QPushButton("Apply", customRow);
    applyButton->setObjectName("TwitchBadgePickerApplyButton");
    applyButton->setCursor(Qt::PointingHandCursor);
    applyButton->setFixedHeight(controlHeight);
    applyButton->setFont(
        getApp()->getFonts()->getFont(FontStyle::UiMedium, scale));

    auto updateApplyState = [this, applyButton, hexInput] {
        applyButton->setEnabled(
            !this->actionInFlight_ &&
            isValidCustomHexColor(normalizeCustomHexInput(hexInput->text())));
    };
    updateApplyState();

    QPointer<QLineEdit> hexInputPtr(hexInput);
    auto openColorPicker = [this, hexInputPtr, currentHex, updatePreview] {
        QColor initial(QStringLiteral("#808080"));
        if (hexInputPtr)
        {
            const auto text = normalizeCustomHexInput(hexInputPtr->text());
            if (isValidCustomHexColor(text))
            {
                initial = QColor(text);
            }
            else if (!currentHex.isEmpty())
            {
                initial = QColor(currentHex);
            }
        }
        else if (!currentHex.isEmpty())
        {
            initial = QColor(currentHex);
        }

        const bool wasAutoPinned = this->ensurePinned();

        auto *dialog = new ColorPickerDialog(initial, this);
        QObject::connect(
            dialog, &ColorPickerDialog::colorConfirmed, this,
            [hexInputPtr, updatePreview](const QColor &selected) {
                if (!selected.isValid() || !hexInputPtr)
                {
                    return;
                }
                hexInputPtr->setText(selected.name(QColor::HexRgb).toUpper());
                updatePreview();
            });
        QObject::connect(dialog, &QObject::destroyed, this,
                         [this, wasAutoPinned] {
                             if (wasAutoPinned)
                             {
                                 this->togglePinned();
                             }
                         });
        dialog->show();
    };

    QObject::connect(colorButton, &QAbstractButton::clicked, this,
                     openColorPicker);
    QObject::connect(
        hexInput, &QLineEdit::textChanged, customRow,
        [hexInput, updatePreview, updateApplyState] {
            const auto raw = hexInput->text();
            const auto normalized = normalizeCustomHexInput(raw);
            if (normalized != raw)
            {
                const auto cursor = hexInput->cursorPosition();
                const auto addedPrefix =
                    !raw.startsWith('#') && normalized.startsWith('#');
                hexInput->blockSignals(true);
                hexInput->setText(normalized);
                hexInput->setCursorPosition(cursor + (addedPrefix ? 1 : 0));
                hexInput->blockSignals(false);
            }
            updatePreview();
            updateApplyState();
        });
    QObject::connect(
        applyButton, &QPushButton::clicked, this, [this, hexInput] {
            const auto text = normalizeCustomHexInput(hexInput->text());
            if (!isValidCustomHexColor(text))
            {
                this->setStatus("Enter a valid hex color (#RRGGBB).", true);
                this->rebuildContent();
                return;
            }
            this->selectColor(text);
        });

    customLayout->addWidget(colorButton, 0, Qt::AlignVCenter);
    customLayout->addWidget(hexInput, 1);
    customLayout->addWidget(applyButton, 0, Qt::AlignVCenter);
    this->contentLayout_->addWidget(customRow);

    if (!selectedPreset && !currentHex.isEmpty() && !this->statusIsError_)
    {
        this->setStatus({});
    }
}

void TwitchBadgePickerDialog::selectColor(const QString &color)
{
    const auto user = getApp()->getAccounts()->twitch.getCurrent();
    if (user->isAnon())
    {
        this->setStatus("You must be logged in to change your color.", true);
        this->rebuildContent();
        return;
    }

    auto colorString = color.trimmed();
    if (colorString.isEmpty())
    {
        return;
    }

    cleanHelixColorName(colorString);

    this->actionInFlight_ = true;
    this->rebuildContent();

    QPointer<TwitchBadgePickerDialog> self = this;
    getHelix()->updateUserChatColor(
        user->getUserId(), colorString,
        [self, colorString, user, channel{this->channel_}] {
            if (!self)
                return;
            self->actionInFlight_ = false;
            self->setStatus({});

            if (colorString.startsWith('#'))
            {
                user->setColor(QColor(colorString));
            }
            else
            {
                const auto hex = helixColorDisplayHex(colorString);
                if (!hex.isEmpty())
                {
                    user->setColor(QColor(hex));
                }
            }

            channel->addSystemMessage(
                QStringLiteral("Your color has been changed to %1.")
                    .arg(colorString));
            self->rebuildContent();
        },
        [self, colorString](auto error, auto message) {
            if (!self)
                return;
            self->actionInFlight_ = false;

            QString errorMessage =
                QStringLiteral("Failed to change color to %1 - ")
                    .arg(colorString);

            switch (error)
            {
                case HelixUpdateUserChatColorError::UserMissingScope: {
                    errorMessage +=
                        "Missing required scope. Re-login with your "
                        "account and try again.";
                }
                break;

                case HelixUpdateUserChatColorError::InvalidColor: {
                    errorMessage +=
                        QStringLiteral("Color must be one of Twitch's "
                                       "supported colors (%1) or a "
                                       "hex code (#000000) if you "
                                       "have Turbo or Prime.")
                            .arg(VALID_HELIX_COLORS.join(", "));
                }
                break;

                case HelixUpdateUserChatColorError::Forwarded: {
                    errorMessage += message + ".";
                }
                break;

                case HelixUpdateUserChatColorError::Unknown:
                default: {
                    errorMessage += "An unknown error has occurred.";
                }
                break;
            }

            self->setStatus(errorMessage, true);
            self->rebuildContent();
        });
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
    this->statusLabel_->setStyleSheet(QStringLiteral("color: %1;").arg(color));
}

void TwitchBadgePickerDialog::selectGlobal(const GqlBadge &badge)
{
    const auto token = this->authTokenOrMessage();
    if (token.isEmpty())
        return;

    const bool clearing = badge.setID.isEmpty();

    this->actionInFlight_ = true;
    this->rebuildContent();

    QPointer<TwitchBadgePickerDialog> self = this;
    TwitchGql::selectGlobalBadge(
        badge.setID, badge.version, token,
        [self, badge, clearing] {
            if (!self)
                return;
            self->actionInFlight_ = false;
            self->badges_.selectedGlobalBadge = clearing ? GqlBadge{} : badge;
            self->channel_->addSystemMessage(
                clearing ? QStringLiteral("Global badge cleared.")
                         : QStringLiteral("Global badge set to: %1")
                               .arg(badge.title));
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
            self->badges_.useCustomChannelBadge = true;
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

void TwitchBadgePickerDialog::deselectChannel()
{
    const auto token = this->authTokenOrMessage();
    if (token.isEmpty())
        return;

    this->actionInFlight_ = true;
    this->rebuildContent();

    QPointer<TwitchBadgePickerDialog> self = this;
    const auto channelId = this->channel_->roomId();

    TwitchGql::deselectChannelBadge(
        channelId, token,
        [self] {
            if (!self)
                return;
            self->actionInFlight_ = false;
            self->badges_.useCustomChannelBadge = false;
            self->channel_->addSystemMessage(
                QStringLiteral("Channel badge disabled."));
            self->rebuildContent();
        },
        [self](const QString &error) {
            if (!self)
                return;
            self->actionInFlight_ = false;
            self->setStatus(
                MoltorinoAuth::normalizeAuthError("deselecting badge", error),
                true);
            self->rebuildContent();
        });
}

void TwitchBadgePickerDialog::setFlairHidden(bool hidden)
{
    const auto token = this->authTokenOrMessage();
    if (token.isEmpty())
        return;

    this->actionInFlight_ = true;
    this->rebuildContent();

    QPointer<TwitchBadgePickerDialog> self = this;

    TwitchGql::setBadgeModifierHidden(
        hidden, token,
        [self](bool actual) {
            if (!self)
                return;
            self->actionInFlight_ = false;
            self->badges_.isBadgeModifierHidden = actual;
            self->channel_->addSystemMessage(
                actual ? QStringLiteral("Badge flair hidden.")
                       : QStringLiteral("Badge flair shown."));
            self->rebuildContent();
        },
        [self](const QString &error) {
            if (!self)
                return;
            self->actionInFlight_ = false;
            self->setStatus(MoltorinoAuth::normalizeAuthError(
                                "updating badge flair", error),
                            true);
            self->rebuildContent();
        });
}

void TwitchBadgePickerDialog::refreshStyle()
{
    auto *fonts = getApp()->getFonts();
    const auto rawScale = this->scale();
    const auto effectiveScale = rawScale;
    const int radius = std::max(1, int(2 * rawScale));
    const int inputPaddingX = std::max(4, int(5 * effectiveScale));
    const int inputMinHeight = std::max(14, int(20 * effectiveScale));
    const int scrollbarWidth = std::max(3, int(4 * effectiveScale));
    const int scrollbarRadius = std::max(1, int(2 * effectiveScale));
    const int scrollbarMinHeight = std::max(12, int(16 * effectiveScale));

    this->headerTitleLabel_->setFont(
        fonts->getFont(FontStyle::UiMediumBold, rawScale * 1.2F));
    this->globalTabButton_->setFont(
        fonts->getFont(FontStyle::UiMedium, effectiveScale));
    this->channelTabButton_->setFont(
        fonts->getFont(FontStyle::UiMedium, effectiveScale));
    this->eventTabButton_->setFont(
        fonts->getFont(FontStyle::UiMedium, effectiveScale));
    this->colorTabButton_->setFont(
        fonts->getFont(FontStyle::UiMedium, effectiveScale));
    if (this->searchInput_ != nullptr)
    {
        this->searchInput_->setFont(
            fonts->getFont(FontStyle::UiMedium, effectiveScale));
    }
    const int customColorHeight = scaledMetric(effectiveScale, 28, 22);
    for (auto *hexInput : this->contentWidget_->findChildren<QLineEdit *>(
             "TwitchBadgePickerHexInput"))
    {
        hexInput->setFixedHeight(customColorHeight);
        hexInput->setFont(
            fonts->getFont(FontStyle::ChatMediumMono, effectiveScale));
    }
    for (auto *applyButton : this->contentWidget_->findChildren<QPushButton *>(
             "TwitchBadgePickerApplyButton"))
    {
        applyButton->setFixedHeight(customColorHeight);
        applyButton->setFont(
            fonts->getFont(FontStyle::UiMedium, effectiveScale));
    }
    for (auto *colorButton :
         this->contentWidget_->findChildren<ColorButton *>(QString()))
    {
        if (colorButton->parent() &&
            colorButton->parent()->objectName() ==
                QStringLiteral("TwitchBadgePickerCustomColorRow"))
        {
            colorButton->setFixedSize(customColorHeight, customColorHeight);
        }
    }

    const int rowPaddingX = scaledMetric(effectiveScale, 8, 4);
    const int rowPaddingY = scaledMetric(effectiveScale, 6, 3);
    const int rowSpacing = scaledMetric(effectiveScale, 8, 4);
    for (auto *row : this->contentWidget_->findChildren<QWidget *>(
             "TwitchBadgePickerSettingRow"))
    {
        if (auto *rowLayout = qobject_cast<QHBoxLayout *>(row->layout()))
        {
            rowLayout->setContentsMargins(rowPaddingX, rowPaddingY, rowPaddingX,
                                          rowPaddingY);
            rowLayout->setSpacing(rowSpacing);
        }
    }
    for (auto *label : this->contentWidget_->findChildren<QLabel *>(
             "TwitchBadgePickerSettingLabel"))
    {
        label->setFont(fonts->getFont(FontStyle::UiMedium, effectiveScale));
    }
    for (auto *button : this->contentWidget_->findChildren<QPushButton *>(
             "TwitchBadgePickerToggleSwitch"))
    {
        static_cast<BadgePickerToggleSwitch *>(button)->updateMetrics(rawScale);
    }

    const int hMargin = contentHorizontalMargin(rawScale);
    const int vMargin = std::max(3, int(5 * rawScale));
    this->headerWidget_->layout()->setContentsMargins(0, 0, 0, 0);
    this->mainLayout_->setContentsMargins(hMargin, vMargin, hMargin, vMargin);
    this->contentLayout_->setContentsMargins(
        0, scaledMetric(effectiveScale, 7, 4), 0,
        scaledMetric(effectiveScale, 8, 4));
    this->contentLayout_->setSpacing(scaledMetric(effectiveScale, 7, 4));
    if (auto *sep = this->findChild<QWidget *>(
            QStringLiteral("TwitchBadgePickerDialogSeparator")))
    {
        sep->setFixedHeight(scaledSeparatorHeight(rawScale));
    }

    const auto *theme = this->theme;
    auto textColor = theme->window.text;
    auto mutedColor = textColor;
    mutedColor.setAlpha(160);
    const auto bg = theme->window.background.name();
    const auto text = textColor.name(QColor::HexArgb);
    const auto border = theme->splits.header.border.name();
    const auto muted = mutedColor.name(QColor::HexArgb);
    const auto inputBg = theme->splits.input.background.name();
    const auto focusedBorder = theme->splits.header.focusedBorder.name();
    const auto hoverBg =
        theme->isLightTheme()
            ? theme->splits.input.background.darker(104).name()
            : theme->splits.input.background.lighter(108).name();

    this->closeButton_->setColor(textColor);

    this->setStyleSheet(QStringLiteral(R"(
        QWidget#TwitchBadgePickerDialogRoot {
            background: %1;
            color: %2;
        }
        QWidget#TwitchBadgePickerHeader {
            background: transparent;
        }
        QFrame#TwitchBadgePickerDialogSeparator,
        QWidget#TwitchBadgePickerDialogSeparator {
            background: %3;
        }
        QScrollArea#TwitchBadgePickerScrollArea {
            background: transparent;
            border: 0;
        }
        QWidget#TwitchBadgePickerDialogContent {
            background: transparent;
            color: %2;
        }
        QLabel#TwitchBadgePickerTitle {
            color: %2;
            font-weight: 700;
        }
        QLabel#TwitchBadgePickerSectionLabel {
            color: %2;
            font-weight: 600;
            padding-top: 4px;
        }
        QLabel#TwitchBadgePickerStatus,
        QLabel#TwitchBadgePickerEmpty {
            color: %4;
        }
        QScrollBar:vertical {
            width: %10px;
            background: transparent;
            margin: 0;
        }
        QScrollBar::handle:vertical {
            background: %3;
            min-height: %12px;
            border-radius: %11px;
        }
        QScrollBar::add-line:vertical,
        QScrollBar::sub-line:vertical,
        QScrollBar::add-page:vertical,
        QScrollBar::sub-page:vertical {
            background: transparent;
            height: 0;
        }
        QPushButton#TwitchBadgePickerTab {
            background: %5;
            color: %2;
            border: 1px solid %3;
            border-radius: %6px;
            padding: 3px 10px;
        }
        QPushButton#TwitchBadgePickerTab:checked {
            background: #9146ff;
            color: #ffffff;
            border-color: #9146ff;
        }
        QPushButton#TwitchBadgePickerTab:hover:!checked {
            background: %8;
            border-color: %7;
        }
        QLineEdit#TwitchBadgePickerSearch {
            background: %5;
            color: %2;
            border: 1px solid %3;
            border-radius: %6px;
            padding: 0 %9px;
            min-height: %13px;
        }
        QLineEdit#TwitchBadgePickerSearch:focus {
            border-color: %7;
        }
        QLineEdit#TwitchBadgePickerHexInput {
            background: %5;
            color: %2;
            border: 1px solid %3;
            border-radius: %6px;
            padding: 0 %9px;
            min-height: %13px;
        }
        QLineEdit#TwitchBadgePickerHexInput:focus {
            border-color: %7;
        }
        QPushButton#TwitchBadgePickerApplyButton {
            background: #9146ff;
            color: #ffffff;
            border: 1px solid #9146ff;
            border-radius: %6px;
            padding: 0 14px;
            min-height: %13px;
            font-weight: 600;
        }
        QPushButton#TwitchBadgePickerApplyButton:hover:enabled {
            background: #a970ff;
            border-color: #a970ff;
        }
        QPushButton#TwitchBadgePickerApplyButton:disabled {
            background: %5;
            color: %4;
            border-color: %3;
            font-weight: 400;
        }
        QWidget#TwitchBadgePickerCustomColorRow {
            background: transparent;
        }
        QWidget#TwitchBadgePickerSettingRow {
            background: %5;
            border: 1px solid %3;
            border-radius: %6px;
        }
        QLabel#TwitchBadgePickerSettingLabel {
            color: %2;
        }
    )")
                            .arg(bg, text, border, muted, inputBg,
                                 QString::number(radius), focusedBorder,
                                 hoverBg, QString::number(inputPaddingX),
                                 QString::number(scrollbarWidth),
                                 QString::number(scrollbarRadius),
                                 QString::number(scrollbarMinHeight),
                                 QString::number(inputMinHeight)));
}

void TwitchBadgePickerDialog::applySizeConstraints()
{
    const int w = std::max(1, int(DEFAULT_DIALOG_SIZE.width() * this->scale()));
    const int h =
        std::max(1, int(DEFAULT_DIALOG_SIZE.height() * this->scale()));
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

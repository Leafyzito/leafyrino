// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/UserBadgesDialog.hpp"

#include "Application.hpp"
#include "messages/Image.hpp"
#include "messages/ImageSet.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Twitch.hpp"
#include "widgets/buttons/Button.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/helper/Line.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QCursor>
#include <QDesktopServices>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>

namespace chatterino {

namespace {

constexpr QSize DEFAULT_DIALOG_SIZE(332, 440);
constexpr int HEADER_SEPARATOR_HEIGHT = 8;
constexpr int BADGE_GRID_SPACING = 4;
constexpr int BADGE_TILE_PADDING = 4;
constexpr QSize BADGE_ICON_SIZE(36, 36);
constexpr float BADGE_IMAGE_SCALE =
    float(BADGE_ICON_SIZE.width()) / float(BADGE_ICON_SIZE.width() / 2);

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

int badgeTileSize()
{
    return BADGE_ICON_SIZE.width() + BADGE_TILE_PADDING * 2;
}

int badgeGridColumnsForWidth(int availableWidth)
{
    if (availableWidth <= 0)
    {
        const int fallbackWidth =
            DEFAULT_DIALOG_SIZE.width() - contentHorizontalMargin(1.0F) * 2;
        availableWidth = fallbackWidth;
    }

    const int stride = badgeTileSize() + BADGE_GRID_SPACING;
    return std::max(1, (availableWidth + BADGE_GRID_SPACING) / stride);
}

bool badgeMatchesSearch(const GqlBadge &badge, const QString &needle)
{
    if (needle.isEmpty())
    {
        return true;
    }

    return badge.title.contains(needle, Qt::CaseInsensitive) ||
           badge.setID.contains(needle, Qt::CaseInsensitive) ||
           badge.description.contains(needle, Qt::CaseInsensitive);
}

QString badgeTooltip(const GqlBadge &badge)
{
    QString tip = badge.title;
    if (!badge.description.isEmpty() && badge.description != badge.title)
    {
        tip += QChar('\n') + badge.description;
    }
    tip += QStringLiteral("\nClick to open in Chat Vault");
    return tip;
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
        this->setToolTip(badgeTooltip(badge));

        this->connections_.managedConnect(
            getApp()->getWindows()->layoutRequested, [this](Channel *) {
                this->refreshImageIfNeeded();
            });
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

        const auto rect = this->rect().adjusted(0, 0, -1, -1);
        paintBadgeTileBackground(painter, rect, this->underMouse(),
                                 this->isDown());

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
    bool attemptRefresh_ = false;
    pajlada::Signals::SignalHolder connections_;
};

}  // namespace

std::vector<QPointer<UserBadgesDialog>> UserBadgesDialog::activeDialogs_;

UserBadgesDialog::UserBadgesDialog(const QString &userLogin,
                                   const QString &channelLogin,
                                   const QString &displayName,
                                   TwitchChannel *channel, QWidget *parent)
    : DraggablePopup(true, parent)
    , userLogin_(userLogin)
    , channelLogin_(channelLogin)
    , displayName_(displayName.isEmpty() ? userLogin : displayName)
    , channel_(channel)
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setObjectName("UserBadgesDialog");
    const auto title = QStringLiteral("%1's Badges").arg(this->displayName_);
    this->setWindowTitle(title);
    this->setScaleIndependentSize(DEFAULT_DIALOG_SIZE);

    auto *container = this->getLayoutContainer();
    container->setObjectName("UserBadgesDialogRoot");
    container->setMouseTracking(true);
    this->mainLayout_ = new QVBoxLayout(container);
    this->mainLayout_->setSpacing(0);

    this->headerWidget_ = new QWidget(container);
    this->headerWidget_->setObjectName("UserBadgesHeader");
    auto *headerLayout = new QHBoxLayout(this->headerWidget_);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(4);

    this->headerTitleLabel_ = new QLabel(title, this->headerWidget_);
    this->headerTitleLabel_->setObjectName("UserBadgesTitle");
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
    separator->setObjectName("UserBadgesDialogSeparator");
    separator->setFixedHeight(scaledSeparatorHeight(this->scale()));
    this->mainLayout_->addWidget(separator);

    this->searchInput_ = new QLineEdit(container);
    this->searchInput_->setObjectName("UserBadgesSearch");
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
    auto *searchRow = new QHBoxLayout();
    searchRow->setContentsMargins(0, scaledMetric(this->scale(), 4, 2), 0,
                                  scaledMetric(this->scale(), 4, 2));
    searchRow->addWidget(this->searchInput_);
    this->mainLayout_->addLayout(searchRow);

    this->scrollArea_ = new QScrollArea(container);
    this->scrollArea_->setObjectName("UserBadgesScrollArea");
    this->scrollArea_->setFrameShape(QFrame::NoFrame);
    this->scrollArea_->setWidgetResizable(true);
    this->scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->scrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    this->scrollArea_->viewport()->setAutoFillBackground(false);
    this->mainLayout_->addWidget(this->scrollArea_, 1);

    this->contentWidget_ = new QWidget();
    this->contentWidget_->setObjectName("UserBadgesDialogContent");
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
    this->applySizeConstraints();
    this->rebuildContent();
}

void UserBadgesDialog::showDialog(const QString &userLogin,
                                  const QString &channelLogin,
                                  const QString &displayName,
                                  TwitchChannel *channel, QWidget *parent)
{
    if (userLogin.isEmpty() || channelLogin.isEmpty())
    {
        return;
    }

    for (auto it = activeDialogs_.begin(); it != activeDialogs_.end();)
    {
        if (it->isNull())
        {
            it = activeDialogs_.erase(it);
            continue;
        }
        if ((*it)->userLogin_.compare(userLogin, Qt::CaseInsensitive) == 0 &&
            (*it)->channelLogin_.compare(channelLogin, Qt::CaseInsensitive) ==
                0)
        {
            (*it)->channel_ = channel;
            (*it)->raise();
            (*it)->activateWindow();
            (*it)->loadBadges(true);
            return;
        }
        ++it;
    }

    auto *dialog = new UserBadgesDialog(userLogin, channelLogin, displayName,
                                        channel, parent);
    activeDialogs_.push_back(dialog);

    QPoint center = QCursor::pos();
    if (parent && parent->window())
    {
        center = parent->window()->geometry().center();
    }

    dialog->show();
    const auto size = dialog->size();
    dialog->showAndMoveTo(center - QPoint(size.width() / 2, size.height() / 2),
                          widgets::BoundsChecking::DesiredPosition);
    dialog->raise();
    dialog->activateWindow();
    dialog->loadBadges(false);
}

void UserBadgesDialog::themeChangedEvent()
{
    DraggablePopup::themeChangedEvent();
    this->refreshStyle();
}

void UserBadgesDialog::scaleChangedEvent(float scale)
{
    DraggablePopup::scaleChangedEvent(scale);
    this->refreshStyle();
    this->applySizeConstraints();
    this->rebuildContent();
}

void UserBadgesDialog::resizeEvent(QResizeEvent *event)
{
    DraggablePopup::resizeEvent(event);

    const int minW = this->minimumWidth();
    const int minH = this->minimumHeight();
    if (this->width() < minW || this->height() < minH)
    {
        this->resize(std::max(this->width(), minW),
                     std::max(this->height(), minH));
    }

    if (this->badgeGridColumns() != this->lastBadgeGridColumns_)
    {
        this->rebuildContent();
    }

    if (this->scrollArea_ != nullptr)
    {
        this->scrollArea_->horizontalScrollBar()->setValue(0);
    }
}

void UserBadgesDialog::showEvent(QShowEvent *event)
{
    DraggablePopup::showEvent(event);

    if (!this->initialFetchDone_)
    {
        QTimer::singleShot(0, this, [this] {
            if (!this->initialFetchDone_)
            {
                this->loadBadges(false);
            }
        });
    }
}

void UserBadgesDialog::loadBadges(bool force)
{
    if (this->badgesLoading_ && !force)
    {
        return;
    }

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

    QPointer<UserBadgesDialog> self = this;

    TwitchGql::getChannelViewerEarnedBadges(
        this->userLogin_, this->channelLogin_, token,
        [self](QVector<GqlBadge> badges) {
            if (!self)
            {
                return;
            }
            self->badgesLoading_ = false;
            self->badgesLoaded_ = true;
            self->badges_ = std::move(badges);
            self->setStatus({});
            self->rebuildContent();
        },
        [self](const QString &error) {
            if (!self)
            {
                return;
            }
            self->badgesLoading_ = false;
            self->setStatus(
                MoltorinoAuth::normalizeAuthError("loading badges", error),
                true);
            self->rebuildContent();
        });
}

void UserBadgesDialog::rebuildContent()
{
    this->clearContent();

    this->statusLabel_ = new QLabel(this->contentWidget_);
    this->statusLabel_->setObjectName("UserBadgesStatus");
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

    if (!this->badgesLoaded_ && this->statusIsError_ &&
        !this->statusText_.isEmpty())
    {
        this->contentLayout_->addStretch(1);
        return;
    }

    const auto needle = this->searchQuery_.trimmed();
    int matchCount = 0;
    for (const auto &badge : this->badges_)
    {
        if (badgeMatchesSearch(badge, needle))
        {
            ++matchCount;
        }
    }

    if (this->badges_.isEmpty())
    {
        if (!(this->statusIsError_ && !this->statusText_.isEmpty()))
        {
            this->setStatus("No badges found.");
        }
        this->contentLayout_->addStretch(1);
        return;
    }

    const auto labelText = matchCount > 0
                               ? QStringLiteral("Badges (%1)").arg(matchCount)
                               : QStringLiteral("Badges");
    auto *label = new QLabel(labelText, this->contentWidget_);
    label->setObjectName("UserBadgesSectionLabel");
    this->contentLayout_->addWidget(label);

    if (matchCount == 0)
    {
        auto *empty =
            new QLabel("No badges match your search.", this->contentWidget_);
        empty->setObjectName("UserBadgesEmpty");
        empty->setWordWrap(true);
        this->contentLayout_->addWidget(empty);
        this->contentLayout_->addStretch(1);
        return;
    }

    const int gridColumns = this->badgeGridColumns();
    this->lastBadgeGridColumns_ = gridColumns;

    auto *gridWidget = new QWidget(this->contentWidget_);
    auto *grid = new QGridLayout(gridWidget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(BADGE_GRID_SPACING);

    int row = 0;
    int col = 0;
    for (const auto &badge : this->badges_)
    {
        if (!badgeMatchesSearch(badge, needle))
        {
            continue;
        }

        auto *tile = new BadgeTileButton(badge, gridWidget);
        QObject::connect(tile, &QPushButton::clicked, this, [this, badge] {
            this->openBadgeInChatVault(badge);
        });
        grid->addWidget(tile, row, col);
        if (++col >= gridColumns)
        {
            col = 0;
            ++row;
        }
    }

    this->contentLayout_->addWidget(gridWidget);
    this->contentLayout_->addStretch(1);
}

void UserBadgesDialog::clearContent()
{
    if (this->contentLayout_ == nullptr)
    {
        return;
    }

    while (QLayoutItem *item = this->contentLayout_->takeAt(0))
    {
        if (auto *widget = item->widget())
        {
            widget->deleteLater();
        }
        delete item;
    }
    this->statusLabel_ = nullptr;
}

void UserBadgesDialog::setStatus(const QString &text, bool error)
{
    this->statusText_ = text;
    this->statusIsError_ = error;

    if (this->statusLabel_ == nullptr)
    {
        return;
    }

    if (text.isEmpty())
    {
        this->statusLabel_->hide();
        this->statusLabel_->clear();
        return;
    }

    this->statusLabel_->setText(text);
    this->statusLabel_->show();
}

int UserBadgesDialog::badgeGridColumns() const
{
    int availableWidth = 0;
    if (this->scrollArea_ != nullptr &&
        this->scrollArea_->viewport() != nullptr)
    {
        availableWidth = this->scrollArea_->viewport()->width();
    }

    if (availableWidth <= 0)
    {
        const float scale = this->scale();
        availableWidth =
            int(std::round(float(this->scaleIndependentWidth()) * scale)) -
            contentHorizontalMargin(scale) * 2;
    }

    return badgeGridColumnsForWidth(availableWidth);
}

void UserBadgesDialog::applySizeConstraints()
{
    const int requiredW =
        std::max(1, int(this->scaleIndependentWidth() * this->scale()));
    const int requiredH =
        std::max(1, int(this->scaleIndependentHeight() * this->scale()));

    const int minW = std::max(this->minimumWidth(), requiredW);
    const int minH = std::max(this->minimumHeight(), requiredH);

    this->setMinimumSize(minW, minH);
    this->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);

    if (this->width() < minW || this->height() < minH)
    {
        this->resize(std::max(this->width(), minW),
                     std::max(this->height(), minH));
    }
}

void UserBadgesDialog::openBadgeInChatVault(const GqlBadge &badge)
{
    if (auto url =
            chatVaultBadgeUrl(badge.setID, badge.version, this->channel_))
    {
        QDesktopServices::openUrl(QUrl(*url));
    }
}

void UserBadgesDialog::refreshStyle()
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
    if (this->searchInput_ != nullptr)
    {
        this->searchInput_->setFont(
            fonts->getFont(FontStyle::UiMedium, effectiveScale));
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
            QStringLiteral("UserBadgesDialogSeparator")))
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

    this->setStyleSheet(
        QStringLiteral(R"(
        QWidget#UserBadgesDialogRoot {
            background: %1;
            color: %2;
        }
        QWidget#UserBadgesHeader {
            background: transparent;
        }
        QFrame#UserBadgesDialogSeparator,
        QWidget#UserBadgesDialogSeparator {
            background: %3;
        }
        QScrollArea#UserBadgesScrollArea {
            background: transparent;
            border: 0;
        }
        QWidget#UserBadgesDialogContent {
            background: transparent;
            color: %2;
        }
        QLabel#UserBadgesTitle {
            color: %2;
            font-weight: 700;
        }
        QLabel#UserBadgesSectionLabel {
            color: %2;
            font-weight: 600;
            padding-top: 4px;
        }
        QLabel#UserBadgesStatus,
        QLabel#UserBadgesEmpty {
            color: %4;
        }
        QScrollBar:vertical {
            width: %8px;
            background: transparent;
            margin: 0;
        }
        QScrollBar::handle:vertical {
            background: %3;
            min-height: %10px;
            border-radius: %9px;
        }
        QScrollBar::add-line:vertical,
        QScrollBar::sub-line:vertical,
        QScrollBar::add-page:vertical,
        QScrollBar::sub-page:vertical {
            background: transparent;
            height: 0;
        }
        QLineEdit#UserBadgesSearch {
            background: %5;
            color: %2;
            border: 1px solid %3;
            border-radius: %6px;
            padding: 0 %7px;
            min-height: %11px;
            selection-background-color: %12;
        }
        QLineEdit#UserBadgesSearch:focus {
            border-color: %13;
        }
    )")
            .arg(bg, text, border, muted, inputBg, QString::number(radius),
                 QString::number(inputPaddingX),
                 QString::number(scrollbarWidth),
                 QString::number(scrollbarRadius),
                 QString::number(scrollbarMinHeight),
                 QString::number(inputMinHeight), hoverBg, focusedBorder));
}

QString UserBadgesDialog::authTokenOrMessage()
{
    QString authError;
    const auto auth = MoltorinoAuth::resolveCurrentUserToken(&authError);
    if (auth.hasToken())
    {
        return auth.token;
    }

    const auto message =
        authError.isEmpty()
            ? MoltorinoAuth::authRequiredMessage("viewing badges")
            : authError;
    this->setStatus(message, true);
    return {};
}

}  // namespace chatterino

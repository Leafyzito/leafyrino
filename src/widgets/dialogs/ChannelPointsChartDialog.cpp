#include "providers/moltorino/MoltorinoFeatureFlags.hpp"

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS

#    include "widgets/dialogs/ChannelPointsChartDialog.hpp"

#    include "Application.hpp"
#    include "providers/twitch/TwitchChannel.hpp"
#    include "singletons/Fonts.hpp"
#    include "singletons/Theme.hpp"
#    include "util/LayoutCreator.hpp"
#    include "util/WidgetHelpers.hpp"
#    include "widgets/buttons/Button.hpp"
#    include "widgets/buttons/SvgButton.hpp"
#    include "widgets/helper/InvisibleSizeGrip.hpp"
#    include "widgets/helper/Line.hpp"
#    include "widgets/dialogs/ChannelPointsChartView.hpp"

#    include <QCursor>
#    include <QGridLayout>
#    include <QGuiApplication>
#    include <QHBoxLayout>
#    include <QLabel>
#    include <QScreen>
#    include <QShowEvent>
#    include <QVBoxLayout>

#    include <algorithm>
#    include <cmath>

namespace {

using namespace chatterino;

constexpr QSize DEFAULT_DIALOG_SIZE(520, 350);
constexpr int HEADER_SEPARATOR_HEIGHT = 8;

float readableFontScale(float scale)
{
    return std::max(0.68F, scale);
}

int scaledSeparatorHeight(float scale)
{
    return std::max(1, int(HEADER_SEPARATOR_HEIGHT * scale));
}

QString channelDisplayName(TwitchChannel *channel)
{
    if (channel == nullptr || channel->getName().isEmpty())
    {
        return QStringLiteral("channel");
    }
    return channel->getName();
}

}  // namespace

namespace chatterino {

QVector<QPointer<ChannelPointsChartDialog>>
    ChannelPointsChartDialog::activeDialogs_;

ChannelPointsChartDialog::ChannelPointsChartDialog(TwitchChannel *channel,
                                                   QWidget *parent)
    : DraggablePopup(true, parent)
    , channel_(channel)
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setObjectName("ChannelPointsChartDialog");
    this->setWindowTitle(QStringLiteral("Channel Points Chart"));
    this->setScaleIndependentSize(DEFAULT_DIALOG_SIZE);

    auto *container = this->getLayoutContainer();
    container->setObjectName("ChannelPointsChartDialogRoot");
    auto layers = LayoutCreator<QWidget>(container)
                      .setLayoutType<QGridLayout>()
                      .withoutMargin();
    this->mainLayout_ = layers.emplace<QVBoxLayout>().getElement();
    this->mainLayout_->setSpacing(0);

    this->headerWidget_ = new QWidget(container);
    this->headerWidget_->setObjectName("ChannelPointsChartDialogHeader");
    auto *headerLayout = new QHBoxLayout(this->headerWidget_);
    const int margin = std::max(7, int(8 * this->scale()));
    headerLayout->setContentsMargins(margin, 3, margin, 2);

    auto *headerTextLayout = new QVBoxLayout();
    headerTextLayout->setContentsMargins(0, 0, 0, 0);
    headerTextLayout->setSpacing(0);

    this->headerTitleLabel_ = new QLabel(this->headerWidget_);
    this->headerTitleLabel_->setObjectName("ChannelPointsChartHeaderTitle");
    headerTextLayout->addWidget(this->headerTitleLabel_);

    this->headerSubtitleLabel_ = new QLabel(this->headerWidget_);
    this->headerSubtitleLabel_->setObjectName("ChannelPointsChartHeaderSubtitle");
    this->headerSubtitleLabel_->setWordWrap(true);
    headerTextLayout->addWidget(this->headerSubtitleLabel_);

    headerLayout->addLayout(headerTextLayout, 1);

    this->pinButton_ = this->createPinButton();
    this->pinButton_->setToolTip(QStringLiteral("Pin chart popup"));
    headerLayout->addWidget(this->pinButton_);

    this->closeButton_ = new SvgButton(
        {
            .dark = ":/buttons/cancel.svg",
            .light = ":/buttons/cancelDark.svg",
        },
        this, QSize{3, 3});
    this->closeButton_->setScaleIndependentSize(18, 18);
    this->closeButton_->setToolTip(QStringLiteral("Close"));
    this->closeButton_->setCursor(Qt::PointingHandCursor);
    QObject::connect(this->closeButton_, &Button::leftClicked, this,
                     &QWidget::close);
    headerLayout->addWidget(this->closeButton_);
    this->mainLayout_->addWidget(this->headerWidget_);

    auto *separator = new Line(false);
    separator->setObjectName("ChannelPointsChartDialogSeparator");
    separator->setFixedHeight(scaledSeparatorHeight(this->scale()));
    this->mainLayout_->addWidget(separator);

    this->chartView_ = new ChannelPointsChartView(container);
    this->chartView_->setSizePolicy(QSizePolicy::Expanding,
                                    QSizePolicy::Expanding);
    this->mainLayout_->addWidget(this->chartView_, 1);

    layers->addWidget(new InvisibleSizeGrip(this), 0, 0,
                      Qt::AlignRight | Qt::AlignBottom);

    if (this->channel_ != nullptr)
    {
        this->channelPointsConnection_ =
            this->channel_->channelPointsChanged.connect([this] {
                this->reloadChart();
            });
    }

    this->refreshHeader();
    this->refreshStyle();
    this->applySizeConstraints();
}

void ChannelPointsChartDialog::showDialog(TwitchChannel *channel,
                                          QWidget *parent)
{
    if (channel == nullptr)
    {
        return;
    }

    DraggablePopup::pinParentIfNeeded(parent);

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
            (*it)->reloadChart();
            return;
        }
        ++it;
    }

    auto *dialog = new ChannelPointsChartDialog(channel, parent);
    activeDialogs_.push_back(dialog);

    QPoint center = QCursor::pos();
    if (parent != nullptr && parent->window() != nullptr)
    {
        center = parent->window()->geometry().center();
    }

    dialog->show();
    const auto size = dialog->size();
    dialog->showAndMoveTo(center - QPoint(size.width() / 2, size.height() / 2),
                          widgets::BoundsChecking::DesiredPosition);
    dialog->raise();
    dialog->activateWindow();
    dialog->reloadChart();
}

void ChannelPointsChartDialog::themeChangedEvent()
{
    DraggablePopup::themeChangedEvent();
    this->refreshStyle();
}

void ChannelPointsChartDialog::scaleChangedEvent(float scale)
{
    DraggablePopup::scaleChangedEvent(scale);
    this->refreshStyle();
    this->applySizeConstraints();
}

void ChannelPointsChartDialog::showEvent(QShowEvent *event)
{
    DraggablePopup::showEvent(event);
    this->applySizeConstraints();
    this->reloadChart();
}

void ChannelPointsChartDialog::refreshHeader()
{
    this->headerTitleLabel_->setText(
        QStringLiteral("%1's channel points")
            .arg(channelDisplayName(this->channel_)));
    this->headerSubtitleLabel_->setText(
        QStringLiteral("Only logged while Leafyrino is open. Dates are shown "
                       "in UTC."));
}

void ChannelPointsChartDialog::refreshStyle()
{
    auto *fonts = getApp()->getFonts();
    const auto headerScale = readableFontScale(this->scale() * 1.15F);
    const auto subtitleScale = readableFontScale(this->scale());
    this->headerTitleLabel_->setFont(
        fonts->getFont(FontStyle::UiMediumBold, headerScale));
    this->headerSubtitleLabel_->setFont(
        fonts->getFont(FontStyle::UiMedium, subtitleScale));

    const auto *theme = this->theme;
    const auto background = theme->window.background.name();
    const auto text = theme->window.text.name(QColor::HexArgb);
    auto mutedText = theme->window.text;
    mutedText.setAlpha(180);

    this->getLayoutContainer()->setStyleSheet(QStringLiteral(R"(
        QWidget#ChannelPointsChartDialogRoot {
            background: %1;
        }
        QLabel#ChannelPointsChartHeaderTitle {
            color: %2;
        }
        QLabel#ChannelPointsChartHeaderSubtitle {
            color: %3;
        }
    )")
                                   .arg(background, text,
                                        mutedText.name(QColor::HexArgb)));

    if (auto *separator =
            this->findChild<QWidget *>("ChannelPointsChartDialogSeparator"))
    {
        separator->setFixedHeight(scaledSeparatorHeight(this->scale()));
    }

    this->chartView_->applyTheme(*theme);
}

void ChannelPointsChartDialog::applySizeConstraints()
{
    const int minWidth =
        std::max(280, int(DEFAULT_DIALOG_SIZE.width() * 0.65F * this->scale()));
    const int minHeight =
        std::max(180, int(DEFAULT_DIALOG_SIZE.height() * 0.65F * this->scale()));
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

void ChannelPointsChartDialog::reloadChart()
{
    if (this->channel_ == nullptr || this->chartView_ == nullptr)
    {
        return;
    }

    this->chartView_->reloadFromStore(this->channel_->roomId());
}

}  // namespace chatterino

#endif

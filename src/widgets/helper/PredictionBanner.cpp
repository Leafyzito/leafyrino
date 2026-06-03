#include "widgets/helper/PredictionBanner.hpp"

#include "Application.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "util/Helpers.hpp"
#include "widgets/dialogs/PredictionDialog.hpp"
#include "widgets/splits/Split.hpp"

#include <QCursor>
#include <QDateTime>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QShowEvent>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>

namespace chatterino {

namespace {
constexpr int BASE_ICON_SIZE = 14;
constexpr int BASE_HEADER_FONT_SIZE = 11;
constexpr int BASE_TITLE_FONT_SIZE = 15;
constexpr int BASE_OUTCOME_FONT_SIZE = 12;
constexpr int BASE_BAR_HEIGHT = 3;
constexpr int BASE_TOP_MARGIN = 4;
constexpr int BASE_BOTTOM_MARGIN = 7;
constexpr int BASE_RIGHT_MARGIN = 8;
constexpr int BASE_HEADER_LEFT_MARGIN = 8;
constexpr int BASE_CONTENT_LEFT_MARGIN = 8;
constexpr int BASE_SPACING = 3;
constexpr int BASE_HEADER_SPACING = 2;
constexpr int BASE_OUTCOME_SPACING = 4;
constexpr int BASE_HEADER_TEXT_BOTTOM_INSET = 0;

float normalizedBannerScale(float value)
{
    if (!std::isfinite(value))
    {
        return 1.F;
    }
    return std::clamp(value, 0.5F, 2.F);
}

float predictionBannerContentScale()
{
    return normalizedBannerScale(
        float(getSettings()->predictionBannerContentScale));
}

QString stateTextForPrediction(const TwitchChannel::PredictionEvent &prediction)
{
    if (prediction.status == "ACTIVE")
    {
        return "LIVE";
    }
    if (prediction.status == "LOCKED")
    {
        return "LOCKED";
    }
    if (prediction.status == "RESOLVED")
    {
        return "RESOLVED";
    }
    if (prediction.status == "CANCELED")
    {
        return "CANCELED";
    }
    return prediction.status;
}

constexpr std::array<const char *, 10> OUTCOME_PALETTE = {{
    "#1f8fff",
    "#e9198b",
    "#16a34a",
    "#f59e0b",
    "#9147ff",
    "#14b8a6",
    "#ef4444",
    "#84cc16",
    "#06b6d4",
    "#f97316",
}};

QColor outcomeColorByIndex(int index)
{
    return QColor(OUTCOME_PALETTE[index % OUTCOME_PALETTE.size()]);
}

QColor outcomeColorForPrediction(
    const TwitchChannel::PredictionOutcome &outcome, int index)
{
    if (outcome.color == "PINK")
        return QColor("#e9198b");
    if (outcome.color == "GREEN")
        return QColor("#16a34a");
    if (outcome.color == "BLUE" && index == 0)
        return QColor("#1f8fff");
    return outcomeColorByIndex(index);
}

QString formatCompactPoints(qlonglong value)
{
    return formatCompactNumber(value);
}

QString formatRemainingTime(qint64 remainingSeconds)
{
    const auto hours = remainingSeconds / 3600;
    const auto minutes = (remainingSeconds % 3600) / 60;
    const auto seconds = remainingSeconds % 60;

    if (hours > 0)
    {
        return QString("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'));
    }

    return QString("%1:%2").arg(minutes).arg(seconds, 2, 10, QChar('0'));
}

QString formatPredictionAge(const QDateTime &dateTime)
{
    if (!dateTime.isValid())
    {
        return {};
    }

    const qint64 seconds =
        std::max<qint64>(0, dateTime.secsTo(QDateTime::currentDateTimeUtc()));
    if (seconds < 60)
    {
        return QString("%1s ago").arg(seconds);
    }

    const qint64 minutes = seconds / 60;
    if (minutes < 60)
    {
        return QString("%1m ago").arg(minutes);
    }

    const qint64 hours = minutes / 60;
    if (hours < 24)
    {
        return QString("%1h ago").arg(hours);
    }

    const qint64 days = hours / 24;
    return QString("%1d ago").arg(days);
}

QString formatOutcomePoints(qlonglong points)
{
    return formatChannelPoints(points);
}

int outcomePercent(qlonglong points, qlonglong totalPoints, int outcomeCount)
{
    if (totalPoints > 0)
    {
        return static_cast<int>((points * 100.0) / totalPoints);
    }
    if (outcomeCount > 0)
    {
        return 100 / outcomeCount;
    }
    return 0;
}

int scaledInt(float value, int minimum = 1)
{
    return std::max(minimum, int(std::round(value)));
}
}  // namespace

PredictionBanner::PredictionBanner(Split *split, QWidget *parent)
    : BaseWidget(parent)
    , split_(split)
{
    this->setObjectName("PredictionBanner");
    this->setCursor(Qt::PointingHandCursor);
    this->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    this->anim_ = new QVariantAnimation(this);
    this->anim_->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(this->anim_, &QVariantAnimation::valueChanged, this,
                     [this](const QVariant &value) {
                         this->update();
                     });

    this->managedConnections_.emplace_back(split->focused.connect([this]() {
        this->update();
    }));
    this->managedConnections_.emplace_back(split->focusLost.connect([this]() {
        this->update();
    }));

    this->rootLayout_ = new QVBoxLayout(this);
    this->rootLayout_->setContentsMargins(0, BASE_TOP_MARGIN, BASE_RIGHT_MARGIN,
                                          BASE_BOTTOM_MARGIN);
    this->rootLayout_->setSpacing(BASE_SPACING);

    this->topLayout_ = new QHBoxLayout();
    this->topLayout_->setContentsMargins(BASE_HEADER_LEFT_MARGIN, 0, 0, 0);
    this->topLayout_->setSpacing(BASE_HEADER_SPACING);

    this->icon_ = new SvgButton(
        {
            .dark = ":/buttons/prediction-darkMode.svg",
            .light = ":/buttons/prediction-lightMode.svg",
        },
        this, QSize(2, 2));
    this->icon_->setFixedSize(BASE_ICON_SIZE, BASE_ICON_SIZE);
    this->icon_->setCursor(Qt::PointingHandCursor);
    auto *iconOpacity = new QGraphicsOpacityEffect(this);
    iconOpacity->setOpacity(0.55);
    this->icon_->setGraphicsEffect(iconOpacity);
    this->topLayout_->addWidget(this->icon_, 0, Qt::AlignBottom);

    this->metadataLabel_ = new QLabel(this);
    this->metadataLabel_->setObjectName("PredictionMetadata");
    this->metadataLabel_->setContentsMargins(0, 0, 0, 0);
    this->metadataLabel_->setAlignment(Qt::AlignLeft | Qt::AlignBottom);
    this->metadataLabel_->setMinimumWidth(0);
    this->metadataLabel_->setSizePolicy(QSizePolicy::Expanding,
                                        QSizePolicy::Preferred);
    this->metadataLabel_->setStyleSheet(
        QString("font-size: %1px;").arg(BASE_HEADER_FONT_SIZE));
    this->topLayout_->addWidget(this->metadataLabel_, 1, Qt::AlignBottom);

    this->timerLabel_ = new QLabel(this);
    this->timerLabel_->setObjectName("PredictionTimer");
    this->timerLabel_->setStyleSheet(
        QString("font-size: %1px; font-weight: bold;")
            .arg(BASE_HEADER_FONT_SIZE));
    this->timerLabel_->setContentsMargins(0, 0, 4, 0);
    this->timerLabel_->setAlignment(Qt::AlignRight | Qt::AlignBottom);
    this->timerLabel_->hide();
    this->topLayout_->addWidget(this->timerLabel_, 0, Qt::AlignBottom);

    this->toggleButton_ = new SvgButton(
        {
            .dark = ":/buttons/switchLight.svg",
            .light = ":/buttons/switchDark.svg",
        },
        this, QSize(2, 2));
    this->toggleButton_->setToolTip("Switch banner");
    this->toggleButton_->setFixedSize(BASE_ICON_SIZE, BASE_ICON_SIZE);
    this->toggleButton_->setCursor(Qt::PointingHandCursor);
    auto *toggleOpacity = new QGraphicsOpacityEffect(this);
    toggleOpacity->setOpacity(0.5);
    this->toggleButton_->setGraphicsEffect(toggleOpacity);
    this->toggleButton_->hide();
    this->topLayout_->addWidget(this->toggleButton_, 0, Qt::AlignBottom);

    this->closeButton_ = new SvgButton(
        {
            .dark = ":/buttons/cancel.svg",
            .light = ":/buttons/cancelDark.svg",
        },
        this, QSize(2, 2));
    this->closeButton_->setToolTip("Dismiss Prediction");
    this->closeButton_->setFixedSize(BASE_ICON_SIZE, BASE_ICON_SIZE);
    this->closeButton_->setCursor(Qt::PointingHandCursor);
    auto *opacity = new QGraphicsOpacityEffect(this);
    opacity->setOpacity(0.5);
    this->closeButton_->setGraphicsEffect(opacity);
    this->topLayout_->addWidget(this->closeButton_, 0, Qt::AlignBottom);

    this->rootLayout_->addLayout(this->topLayout_);

    this->contentLayout_ = new QVBoxLayout();
    this->contentLayout_->setContentsMargins(BASE_CONTENT_LEFT_MARGIN, 0, 0, 0);
    this->contentLayout_->setSpacing(2);

    this->titleLabel_ = new QLabel(this);
    this->titleLabel_->setObjectName("PredictionTitle");
    this->titleLabel_->setContentsMargins(0, 0, 0, 0);
    this->titleLabel_->setWordWrap(false);
    this->titleLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    this->titleLabel_->setMinimumWidth(0);
    this->titleLabel_->setSizePolicy(QSizePolicy::Ignored,
                                     QSizePolicy::Preferred);
    this->titleLabel_->setStyleSheet(
        QString("font-size: %1px; font-weight: bold;")
            .arg(BASE_TITLE_FONT_SIZE));
    this->contentLayout_->addWidget(this->titleLabel_);

    this->outcomeRowLayout_ = new QHBoxLayout();
    this->outcomeRowLayout_->setContentsMargins(0, 0, 0, 0);
    this->outcomeRowLayout_->setSpacing(BASE_OUTCOME_SPACING);

    this->leftOutcomeLabel_ = new QLabel(this);
    this->leftOutcomeLabel_->setObjectName("PredictionOutcomeLeft");
    this->leftOutcomeLabel_->setContentsMargins(0, 0, 0, 0);
    this->leftOutcomeLabel_->setTextFormat(Qt::RichText);
    this->leftOutcomeLabel_->setWordWrap(false);
    this->leftOutcomeLabel_->setMinimumWidth(0);
    this->leftOutcomeLabel_->setSizePolicy(QSizePolicy::Ignored,
                                           QSizePolicy::Preferred);
    this->leftOutcomeLabel_->setStyleSheet(
        QString("font-size: %1px;").arg(BASE_OUTCOME_FONT_SIZE));
    this->outcomeRowLayout_->addWidget(this->leftOutcomeLabel_, 1);

    this->rightOutcomeLabel_ = new QLabel(this);
    this->rightOutcomeLabel_->setObjectName("PredictionOutcomeRight");
    this->rightOutcomeLabel_->setContentsMargins(0, 0, 0, 0);
    this->rightOutcomeLabel_->setTextFormat(Qt::RichText);
    this->rightOutcomeLabel_->setWordWrap(false);
    this->rightOutcomeLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    this->rightOutcomeLabel_->setMinimumWidth(0);
    this->rightOutcomeLabel_->setSizePolicy(QSizePolicy::Ignored,
                                            QSizePolicy::Preferred);
    this->rightOutcomeLabel_->setStyleSheet(
        QString("font-size: %1px;").arg(BASE_OUTCOME_FONT_SIZE));
    this->outcomeRowLayout_->addWidget(this->rightOutcomeLabel_, 1);
    this->contentLayout_->addLayout(this->outcomeRowLayout_);

    this->outcomeBar_ = new QWidget(this);
    this->outcomeBar_->setAttribute(Qt::WA_TransparentForMouseEvents);
    this->outcomeBar_->setFixedHeight(BASE_BAR_HEIGHT);

    this->rootLayout_->addLayout(this->contentLayout_);

    this->updateTimer_ = new QTimer(this);
    this->updateTimer_->setInterval(1000);
    this->connect(this->updateTimer_, &QTimer::timeout, this,
                  &PredictionBanner::updateTimer);

    this->connect(this->closeButton_, &Button::leftClicked, [this]() {
        if (this->prediction_)
        {
            this->dismissedPredictionKey_ =
                this->dismissalKey(*this->prediction_);
        }
        this->hide();
        this->dismissed.invoke();
    });

    this->connect(this->toggleButton_, &Button::leftClicked, [this]() {
        this->toggleBannerRequested.invoke();
    });

    this->connect(this->icon_, &Button::leftClicked, [this]() {
        this->openPredictionDialog();
    });

    getSettings()->pinBannerBackgroundColor.connect(
        [this](const QString &, auto) {
            this->update();
        },
        this->managedConnections_);

    getSettings()->predictionBannerContentScale.connect(
        [this](const float &, auto) {
            this->scaleChangedEvent(this->scale());
            this->updateGeometry();
        },
        this->managedConnections_);

    this->hide();
}

bool PredictionBanner::hasPrediction() const
{
    if (!this->prediction_.has_value())
    {
        return false;
    }
    return this->dismissedPredictionKey_ !=
           this->dismissalKey(*this->prediction_);
}

void PredictionBanner::setToggleButtonVisible(bool visible)
{
    this->toggleButton_->setVisible(visible);
}

void PredictionBanner::setPrediction(
    const std::optional<TwitchChannel::PredictionEvent> &prediction,
    TwitchChannel *channel)
{
    if (!prediction || !channel || !getSettings()->enablePredictions)
    {
        this->updateTimer_->stop();
        this->prediction_ = std::nullopt;
        this->twitchChannel_ = nullptr;
        this->metadataLabel_->clear();
        this->titleLabel_->clear();
        this->leftOutcomeLabel_->clear();
        this->rightOutcomeLabel_->clear();
        this->timerLabel_->clear();
        this->outcomeBar_->hide();
        this->hide();
        return;
    }

    this->prediction_ = prediction;
    this->twitchChannel_ = channel;

    std::vector<double> newFractions;
    qlonglong totalPoints = 0;
    for (const auto &outcome : prediction->outcomes)
    {
        totalPoints += outcome.totalPoints;
    }
    const double fallbackFraction =
        prediction->outcomes.empty() ? 0.0 : 1.0 / prediction->outcomes.size();
    for (const auto &outcome : prediction->outcomes)
    {
        newFractions.push_back(totalPoints > 0
                                   ? static_cast<double>(outcome.totalPoints) /
                                         totalPoints
                                   : fallbackFraction);
    }

    if (this->currentFractions_.size() != newFractions.size())
    {
        this->currentFractions_ = newFractions;
        this->targetFractions_ = newFractions;
        this->previousFractions_ = newFractions;
        this->anim_->stop();
    }
    else
    {
        double progress = this->anim_->state() == QAbstractAnimation::Running
                              ? this->anim_->currentValue().toDouble()
                              : 1.0;
        for (size_t i = 0; i < this->currentFractions_.size(); ++i)
        {
            this->previousFractions_[i] =
                this->previousFractions_[i] +
                (this->targetFractions_[i] - this->previousFractions_[i]) *
                    progress;
        }
        this->targetFractions_ = newFractions;
        this->anim_->stop();
        this->anim_->setDuration(400);
        this->anim_->setStartValue(0.0);
        this->anim_->setEndValue(1.0);
        this->anim_->start();
    }

    this->updateLayout();
    this->updateTimer();

    if (prediction->status == "ACTIVE" && this->predictionEndsAt().isValid())
    {
        this->updateTimer_->start();
    }
    else
    {
        this->updateTimer_->stop();
    }

    if (this->dismissedPredictionKey_ != this->dismissalKey(*prediction))
    {
        const int autoDismiss = getSettings()->predictionAutoDismissSeconds;
        if (autoDismiss > 0 && (prediction->status == "RESOLVED" ||
                                prediction->status == "CANCELED"))
        {
            QTimer::singleShot(autoDismiss * 1000, this, [this] {
                if (this->prediction_ &&
                    (this->prediction_->status == "RESOLVED" ||
                     this->prediction_->status == "CANCELED"))
                {
                    this->dismissedPredictionKey_ =
                        this->dismissalKey(*this->prediction_);
                    this->hide();
                    this->dismissed.invoke();
                }
            });
        }
    }
    else
    {
        this->hide();
    }
}

void PredictionBanner::updateLayout()
{
    if (!this->prediction_)
    {
        return;
    }

    const auto &prediction = *this->prediction_;
    const auto locale = QLocale();
    const auto textColor = this->theme->splits.header.text;
    auto mutedText = textColor;
    mutedText.setAlpha(170);

    QString winnerTitle;
    qlonglong totalPoints = 0;
    for (const auto &out : prediction.outcomes)
    {
        totalPoints += out.totalPoints;
        if (!prediction.winningOutcomeId.isEmpty() &&
            out.id == prediction.winningOutcomeId)
        {
            winnerTitle = out.title;
        }
    }

    QString metadata;
    if (prediction.selfPoints > 0)
    {
        QString betOutcomeTitle;
        for (const auto &out : prediction.outcomes)
        {
            if (out.id == prediction.selfOutcomeId)
            {
                betOutcomeTitle = out.title;
                break;
            }
        }
        if (betOutcomeTitle.isEmpty())
        {
            betOutcomeTitle = "an outcome";
        }
        metadata = QString("You bet %1 on %2")
                       .arg(formatChannelPoints(prediction.selfPoints),
                            betOutcomeTitle);
    }
    else if (prediction.status == "RESOLVED" &&
             !prediction.endedByName.isEmpty())
    {
        metadata = QString("Resolved by %1").arg(prediction.endedByName);
    }
    else if (prediction.status == "CANCELED" &&
             !prediction.endedByName.isEmpty())
    {
        metadata = QString("Canceled by %1").arg(prediction.endedByName);
    }
    else if (prediction.status == "LOCKED" &&
             !prediction.lockedByName.isEmpty())
    {
        metadata = QString("Locked by %1").arg(prediction.lockedByName);
    }
    else if (!prediction.createdByName.isEmpty())
    {
        metadata = QString("Prediction by %1").arg(prediction.createdByName);
    }
    else
    {
        metadata = "Prediction";
    }
    const auto predictionAge = formatPredictionAge(prediction.createdAt);
    if (!predictionAge.isEmpty())
    {
        metadata += QString::fromUtf8(" \xc2\xb7 %1").arg(predictionAge);
    }
    this->metadataLabel_->setText(metadata);

    QString displayTitle = prediction.title;
    const int titleWidth = this->titleLabel_->width();
    if (titleWidth > 16)
    {
        displayTitle =
            QFontMetrics(this->titleLabel_->font())
                .elidedText(prediction.title, Qt::ElideRight, titleWidth - 4);
    }
    this->titleLabel_->setText(displayTitle);

    const bool isResolved = !prediction.winningOutcomeId.isEmpty();
    const auto outcomeCount = static_cast<int>(prediction.outcomes.size());
    const bool multiOutcome = outcomeCount > 2;

    if (multiOutcome)
    {
        int leadIdx = 0;
        qlonglong leadPts = 0;
        for (int i = 0; i < outcomeCount; ++i)
        {
            if (prediction.outcomes[i].totalPoints > leadPts)
            {
                leadPts = prediction.outcomes[i].totalPoints;
                leadIdx = i;
            }
        }
        const auto &leader = prediction.outcomes[leadIdx];
        const int leadPct =
            outcomePercent(leader.totalPoints, totalPoints, outcomeCount);
        const QColor leadColor = outcomeColorByIndex(leadIdx);

        if (isResolved)
        {
            int winIdx = -1;
            for (int i = 0; i < outcomeCount; ++i)
            {
                if (prediction.outcomes[i].id == prediction.winningOutcomeId)
                {
                    winIdx = i;
                    break;
                }
            }
            if (winIdx >= 0)
            {
                const auto &winner = prediction.outcomes[winIdx];
                const QColor wColor = outcomeColorByIndex(winIdx);
                this->leftOutcomeLabel_->setText(
                    QString(
                        "<span style=\"color:#22c55e;font-weight:700;\">"
                        "Winner:</span> "
                        "<span style=\"color:%1;font-weight:700;\">%2</span>")
                        .arg(wColor.name(), winner.title.toHtmlEscaped()));
            }
            else
            {
                this->leftOutcomeLabel_->setText(
                    QString(
                        "<span style=\"color:%1;font-weight:700;\">%2</span>"
                        " <span style=\"color:#999;\">(%3%)</span>")
                        .arg(leadColor.name(), leader.title.toHtmlEscaped())
                        .arg(leadPct));
            }
        }
        else
        {
            this->leftOutcomeLabel_->setText(
                QString("<span style=\"color:#999;\">Leading:</span> "
                        "<span style=\"color:%1;font-weight:700;\">%2</span>"
                        " <span style=\"color:#999;\">(%3%)</span>")
                    .arg(leadColor.name(), leader.title.toHtmlEscaped())
                    .arg(leadPct));
        }

        this->rightOutcomeLabel_->setText(
            QString("<span style=\"color:#999;\">Total:</span> "
                    "<span style=\"color:#999;\">%1 pts</span>")
                .arg(formatCompactPoints(totalPoints)));
        this->rightOutcomeLabel_->show();
    }
    else if (outcomeCount > 0)
    {
        const auto &leftOutcome = prediction.outcomes.front();
        const int leftPercent =
            outcomePercent(leftOutcome.totalPoints, totalPoints, outcomeCount);
        const bool leftWon =
            isResolved && leftOutcome.id == prediction.winningOutcomeId;

        QString leftName = leftOutcome.title.toHtmlEscaped();
        if (leftWon)
        {
            leftName =
                QString("<span style=\"color:#22c55e;font-weight:700;"
                        "\">WINNER</span> "
                        "<span style=\"color:%1;font-weight:700;\">%2</span>")
                    .arg(outcomeColorForPrediction(leftOutcome, 0).name(),
                         leftName);
        }
        else
        {
            leftName =
                QString("<span style=\"color:%1;font-weight:700;\">%2</span>")
                    .arg(outcomeColorForPrediction(leftOutcome, 0).name(),
                         leftName);
        }

        QString leftStats =
            QString("%1 pts").arg(formatOutcomePoints(leftOutcome.totalPoints));
        if (totalPoints > 0)
        {
            leftStats += QString(" (%1%)").arg(leftPercent);
        }
        this->leftOutcomeLabel_->setText(
            QString("%1 <span style=\"color:#999;\">%2</span>")
                .arg(leftName, leftStats.toHtmlEscaped()));

        if (outcomeCount > 1)
        {
            const auto &rightOutcome = prediction.outcomes.back();
            const int rightPercent = outcomePercent(rightOutcome.totalPoints,
                                                    totalPoints, outcomeCount);
            const bool rightWon =
                isResolved && rightOutcome.id == prediction.winningOutcomeId;

            QString rightName = rightOutcome.title.toHtmlEscaped();
            if (rightWon)
            {
                rightName =
                    QString(
                        "<span style=\"color:%1;font-weight:700;\">%2</span> "
                        "<span style=\"color:#22c55e;font-weight:700;"
                        "\">WINNER</span>")
                        .arg(outcomeColorForPrediction(rightOutcome, 1).name(),
                             rightName);
            }
            else
            {
                rightName =
                    QString(
                        "<span style=\"color:%1;font-weight:700;\">%2</span>")
                        .arg(outcomeColorForPrediction(rightOutcome, 1).name(),
                             rightName);
            }

            QString rightStats = QString("%1 pts").arg(
                formatOutcomePoints(rightOutcome.totalPoints));
            if (totalPoints > 0)
            {
                rightStats.prepend(QString("(%1%) ").arg(rightPercent));
            }
            this->rightOutcomeLabel_->setText(
                QString("<span style=\"color:#999;\">%1</span> %2")
                    .arg(rightStats.toHtmlEscaped(), rightName));
            this->rightOutcomeLabel_->show();
        }
        else
        {
            this->rightOutcomeLabel_->clear();
            this->rightOutcomeLabel_->hide();
        }
    }
    else
    {
        this->leftOutcomeLabel_->clear();
        this->rightOutcomeLabel_->clear();
        this->rightOutcomeLabel_->hide();
    }

    this->outcomeBar_->setVisible(outcomeCount > 0);
    this->update();
}

void PredictionBanner::updateTimer()
{
    if (!this->prediction_)
    {
        return;
    }

    if (this->prediction_->status == "ACTIVE")
    {
        auto now = QDateTime::currentDateTimeUtc();
        const auto endsAt = this->predictionEndsAt();
        if (!endsAt.isValid())
        {
            this->timerLabel_->setText("LIVE");
            this->timerLabel_->show();
            this->timerLabel_->setStyleSheet(
                QString("font-size: %1px; color: #9147ff; font-weight: 700;")
                    .arg(this->headerFontSize_));
            return;
        }

        qint64 remaining = now.secsTo(endsAt);
        auto timerColor = QColor("#ef4444");

        if (remaining > 0)
        {
            if (remaining > 300)
            {
                timerColor = QColor("#f59e0b");
            }

            this->timerLabel_->setText(
                QString("%1 left").arg(formatRemainingTime(remaining)));
            this->timerLabel_->show();
        }
        else
        {
            this->timerLabel_->setText("Closing soon");
            this->timerLabel_->show();
            timerColor = QColor("#f59e0b");
        }

        this->timerLabel_->setStyleSheet(
            QString("font-size: %1px; color: %2; font-weight: 700;")
                .arg(this->headerFontSize_)
                .arg(timerColor.name()));
    }
    else
    {
        if (this->prediction_->status == "LOCKED")
        {
            this->timerLabel_->setText(
                stateTextForPrediction(*this->prediction_));
            this->timerLabel_->show();
            this->timerLabel_->setStyleSheet(
                QString("font-size: %1px; color: #d99024; font-weight: 700;")
                    .arg(this->headerFontSize_));
        }
        else if (this->prediction_->status == "RESOLVED")
        {
            this->timerLabel_->setText(
                stateTextForPrediction(*this->prediction_));
            this->timerLabel_->show();
            this->timerLabel_->setStyleSheet(
                QString("font-size: %1px; color: #22c55e; font-weight: 700;")
                    .arg(this->headerFontSize_));
        }
        else if (this->prediction_->status == "CANCELED")
        {
            this->timerLabel_->setText(
                stateTextForPrediction(*this->prediction_));
            this->timerLabel_->show();
            this->timerLabel_->setStyleSheet(
                QString("font-size: %1px; color: #9ca3af; font-weight: 700;")
                    .arg(this->headerFontSize_));
        }
        else
        {
            this->timerLabel_->clear();
            this->timerLabel_->hide();
        }
    }
}

void PredictionBanner::showEvent(QShowEvent *event)
{
    BaseWidget::showEvent(event);
    this->scaleChangedEvent(this->scale());
}

void PredictionBanner::scaleChangedEvent(float scale)
{
    BaseWidget::scaleChangedEvent(scale);

    int iconSize = scaledInt(BASE_ICON_SIZE * scale);
    this->headerFontSize_ = scaledInt(BASE_HEADER_FONT_SIZE * scale);
    const float contentScale = predictionBannerContentScale();
    this->titleFontSize_ =
        scaledInt(BASE_TITLE_FONT_SIZE * scale * contentScale);
    this->outcomeFontSize_ =
        scaledInt(BASE_OUTCOME_FONT_SIZE * scale * contentScale);

    this->icon_->setFixedSize(iconSize, iconSize);
    this->closeButton_->setFixedSize(iconSize, iconSize);
    this->toggleButton_->setFixedSize(iconSize, iconSize);
    this->outcomeBar_->setFixedHeight(scaledInt(BASE_BAR_HEIGHT * scale, 2));

    if (this->rootLayout_ != nullptr)
    {
        this->rootLayout_->setContentsMargins(
            0, scaledInt(BASE_TOP_MARGIN * scale, 0),
            scaledInt(BASE_RIGHT_MARGIN * scale, 0),
            scaledInt(BASE_BOTTOM_MARGIN * scale, 0));
        this->rootLayout_->setSpacing(scaledInt(BASE_SPACING * scale, 0));
    }
    if (this->topLayout_ != nullptr)
    {
        this->topLayout_->setContentsMargins(
            scaledInt(BASE_HEADER_LEFT_MARGIN * scale, 0), 0, 0, 0);
        this->topLayout_->setSpacing(scaledInt(BASE_HEADER_SPACING * scale, 0));
    }
    const int textBottomInset =
        scaledInt(BASE_HEADER_TEXT_BOTTOM_INSET * scale, 0);
    this->metadataLabel_->setContentsMargins(0, 0, 0, textBottomInset);
    this->timerLabel_->setContentsMargins(0, 0, scaledInt(4 * scale, 0),
                                          textBottomInset);
    if (this->contentLayout_ != nullptr)
    {
        this->contentLayout_->setContentsMargins(
            scaledInt(BASE_CONTENT_LEFT_MARGIN * scale, 0), 0, 0, 0);
        this->contentLayout_->setSpacing(scaledInt(2 * scale));
    }
    if (this->outcomeRowLayout_ != nullptr)
    {
        this->outcomeRowLayout_->setSpacing(
            scaledInt(BASE_OUTCOME_SPACING * scale, 0));
    }

    this->updateLabelStyles();

    if (this->prediction_)
    {
        this->updateLayout();
        this->updateTimer();
    }
}

void PredictionBanner::themeChangedEvent()
{
    this->updateLabelStyles();

    this->icon_->setColor(this->theme->splits.header.text);
    this->closeButton_->setColor(this->theme->splits.header.text);
    if (auto *eff = qobject_cast<QGraphicsOpacityEffect *>(
            this->icon_->graphicsEffect()))
    {
        eff->setOpacity(0.55);
    }
    if (this->prediction_)
    {
        this->updateLayout();
        this->updateTimer();
    }
    this->update();
}

void PredictionBanner::updateLabelStyles()
{
    QColor textColor = this->theme->splits.header.text;
    auto dimColor = QString("rgba(%1, %2, %3, 0.55)")
                        .arg(textColor.red())
                        .arg(textColor.green())
                        .arg(textColor.blue());
    int hfs = this->headerFontSize_;
    int tfs = this->titleFontSize_;
    int ofs = this->outcomeFontSize_;

    this->metadataLabel_->setStyleSheet(
        QString("font-size: %1px; color: %2;").arg(hfs).arg(dimColor));
    this->titleLabel_->setStyleSheet(
        QString("font-size: %1px; font-weight: bold; color: %2;")
            .arg(tfs)
            .arg(textColor.name()));
    this->leftOutcomeLabel_->setStyleSheet(
        QString("font-size: %1px;").arg(ofs));
    this->rightOutcomeLabel_->setStyleSheet(
        QString("font-size: %1px;").arg(ofs));
    this->timerLabel_->setStyleSheet(QString("font-size: %1px;").arg(hfs));
}

void PredictionBanner::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);

    QString customBg = getSettings()->pinBannerBackgroundColor;
    QColor background;
    if (!customBg.isEmpty() && QColor::isValidColorName(customBg))
    {
        background = QColor(customBg);
    }
    else
    {
        background = this->theme->splits.header.background;
    }
    QColor border = this->split_->hasFocus()
                        ? this->theme->splits.header.focusedBorder
                        : this->theme->splits.header.border;

    painter.fillRect(this->rect(), background);
    painter.setPen(border);
    painter.drawRect(0, 0, this->width() - 1, this->height() - 1);

    if (!this->prediction_ || !this->outcomeBar_->isVisible())
    {
        return;
    }

    const QRect barRect = this->outcomeBar_->geometry();
    if (barRect.width() <= 0 || barRect.height() <= 0)
    {
        return;
    }

    painter.setRenderHint(QPainter::Antialiasing, false);

    auto baseBarColor = this->theme->splits.header.border;
    baseBarColor.setAlpha(80);
    painter.fillRect(barRect, baseBarColor);

    qlonglong totalPoints = 0;
    for (const auto &outcome : this->prediction_->outcomes)
    {
        totalPoints += outcome.totalPoints;
    }

    const int outcomeCount =
        static_cast<int>(this->prediction_->outcomes.size());
    if (outcomeCount == 0)
    {
        return;
    }

    const double fallbackFraction = 1.0 / outcomeCount;
    int currentLeft = barRect.left();
    double cumulativeFraction = 0.0;

    double animProgress = this->anim_->state() == QAbstractAnimation::Running
                              ? this->anim_->currentValue().toDouble()
                              : 1.0;

    for (int i = 0; i < outcomeCount; ++i)
    {
        const auto &outcome = this->prediction_->outcomes.at(i);

        double oldF = (i < static_cast<int>(this->previousFractions_.size()))
                          ? this->previousFractions_[i]
                          : fallbackFraction;
        double targetF = (i < static_cast<int>(this->targetFractions_.size()))
                             ? this->targetFractions_[i]
                             : fallbackFraction;
        double fraction = oldF + (targetF - oldF) * animProgress;

        cumulativeFraction += fraction;

        const int nextLeft = (i == outcomeCount - 1)
                                 ? (barRect.left() + barRect.width())
                                 : (barRect.left() + qRound(cumulativeFraction *
                                                            barRect.width()));

        if (nextLeft > currentLeft)
        {
            painter.fillRect(QRect(currentLeft, barRect.top(),
                                   nextLeft - currentLeft, barRect.height()),
                             outcomeColorForPrediction(outcome, i));
        }
        currentLeft = nextLeft;
    }
}

void PredictionBanner::resizeEvent(QResizeEvent *event)
{
    BaseWidget::resizeEvent(event);

    const int barH = this->outcomeBar_->height();
    this->outcomeBar_->setGeometry(0, this->height() - barH, this->width(),
                                   barH);

    if (this->prediction_)
    {
        this->updateLayout();
    }
}

void PredictionBanner::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        this->openPredictionDialog();
        event->accept();
        return;
    }

    BaseWidget::mousePressEvent(event);
}

void PredictionBanner::openPredictionDialog()
{
    if (this->twitchChannel_ == nullptr)
    {
        return;
    }

    this->split_->setFocus(Qt::MouseFocusReason);

    PredictionDialog::showDialog(this->twitchChannel_, this->split_,
                                 this->prediction_);
}

QDateTime PredictionBanner::predictionEndsAt() const
{
    if (!this->prediction_)
    {
        return {};
    }

    if (this->prediction_->lockedAt.has_value() &&
        this->prediction_->lockedAt->isValid())
    {
        return *this->prediction_->lockedAt;
    }

    if (this->prediction_->createdAt.isValid() &&
        this->prediction_->predictionWindowSeconds > 0)
    {
        return this->prediction_->createdAt.addSecs(
            this->prediction_->predictionWindowSeconds);
    }

    return {};
}

QString PredictionBanner::dismissalKey(
    const TwitchChannel::PredictionEvent &prediction) const
{
    return prediction.id + ':' + prediction.status;
}

}  // namespace chatterino

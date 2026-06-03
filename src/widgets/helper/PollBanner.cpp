#include "widgets/helper/PollBanner.hpp"

#include "Application.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/dialogs/PollDialog.hpp"
#include "widgets/splits/Split.hpp"

#include <QDateTime>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QShowEvent>
#include <QStringList>
#include <QVariantAnimation>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace chatterino {

namespace {
constexpr int BASE_ICON_SIZE = 14;
constexpr int BASE_HEADER_FONT_SIZE = 11;
constexpr int BASE_TITLE_FONT_SIZE = 15;
constexpr int BASE_SUMMARY_FONT_SIZE = 12;
constexpr int BASE_BAR_HEIGHT = 3;
constexpr int BASE_TOP_MARGIN = 4;
constexpr int BASE_BOTTOM_MARGIN = 7;
constexpr int BASE_RIGHT_MARGIN = 8;
constexpr int BASE_HEADER_LEFT_MARGIN = 8;
constexpr int BASE_CONTENT_LEFT_MARGIN = 8;
constexpr int BASE_SPACING = 3;
constexpr int BASE_HEADER_SPACING = 2;
constexpr int BASE_HEADER_TEXT_BOTTOM_INSET = 0;

float clampBannerScale(float value)
{
    if (!std::isfinite(value))
    {
        return 1.F;
    }
    return std::clamp(value, 0.5F, 2.F);
}

float pollBannerContentScale()
{
    return clampBannerScale(float(getSettings()->pollBannerContentScale));
}

constexpr std::array<const char *, 5> POLL_TONES = {{
    "#4ade80",
    "#139a48",
    "#0b7134",
    "#064e24",
    "#032e16",
}};
constexpr const char *POLL_ENDED_WINNER_TONE = "#22c55e";
constexpr const char *POLL_ENDED_TRAILING_TONE = "#3f3f46";

QString formatCompactVotes(qlonglong value)
{
    if (value >= 1000000)
    {
        const double millions = value / 1000000.0;
        return QString::number(millions, 'f', millions >= 10.0 ? 0 : 1) + "m";
    }
    if (value >= 1000)
    {
        const double thousands = value / 1000.0;
        return QString::number(thousands, 'f', thousands >= 10.0 ? 0 : 1) + "k";
    }
    return QString::number(value);
}

QString formatRemainingPollTime(qint64 remainingSeconds)
{
    const auto minutes = remainingSeconds / 60;
    const auto seconds = remainingSeconds % 60;
    return QString("%1:%2").arg(minutes).arg(seconds, 2, 10, QChar('0'));
}

QString formatPollAge(const QDateTime &dateTime)
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

    return QString("%1d ago").arg(hours / 24);
}

int outcomePercent(int value, int total, int outcomeCount)
{
    if (total > 0)
    {
        return static_cast<int>((value * 100.0) / total);
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

bool pollIsActive(const TwitchChannel::PollEvent &poll)
{
    return poll.status.compare("ACTIVE", Qt::CaseInsensitive) == 0;
}

int maxPollVotes(const TwitchChannel::PollEvent &poll)
{
    int maxVotes = 0;
    for (const auto &choice : poll.choices)
    {
        maxVotes = std::max(maxVotes, choice.totalVotes);
    }
    return maxVotes;
}

QColor pollToneForChoice(const TwitchChannel::PollEvent &poll, int index,
                         bool votingEnded)
{
    if (index < 0 || index >= static_cast<int>(poll.choices.size()))
    {
        return QColor(POLL_TONES.back());
    }

    const int choiceVotes = poll.choices.at(size_t(index)).totalVotes;
    if (votingEnded)
    {
        if (poll.totalVotes > 0 && choiceVotes == maxPollVotes(poll))
        {
            return QColor(POLL_ENDED_WINNER_TONE);
        }
        return QColor(POLL_ENDED_TRAILING_TONE);
    }

    if (poll.totalVotes <= 0)
    {
        return QColor(POLL_TONES[size_t(index % int(POLL_TONES.size()))]);
    }

    std::vector<int> strongerCounts;
    strongerCounts.reserve(poll.choices.size());
    for (const auto &choice : poll.choices)
    {
        if (choice.totalVotes > choiceVotes &&
            std::find(strongerCounts.begin(), strongerCounts.end(),
                      choice.totalVotes) == strongerCounts.end())
        {
            strongerCounts.push_back(choice.totalVotes);
        }
    }

    const auto rank = std::min(strongerCounts.size(), POLL_TONES.size() - 1);
    return QColor(POLL_TONES[rank]);
}
}  // namespace

PollBanner::PollBanner(Split *split, QWidget *parent)
    : BaseWidget(parent)
    , split_(split)
{
    this->setObjectName("PollBanner");
    this->setCursor(Qt::PointingHandCursor);
    this->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    this->anim_ = new QVariantAnimation(this);
    this->anim_->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(this->anim_, &QVariantAnimation::valueChanged, this,
                     [this](const QVariant &) {
                         this->update();
                     });

    this->rootLayout_ = new QVBoxLayout(this);
    this->rootLayout_->setContentsMargins(0, BASE_TOP_MARGIN, BASE_RIGHT_MARGIN,
                                          BASE_BOTTOM_MARGIN);
    this->rootLayout_->setSpacing(BASE_SPACING);

    this->topLayout_ = new QHBoxLayout();
    this->topLayout_->setContentsMargins(BASE_HEADER_LEFT_MARGIN, 0, 0, 0);
    this->topLayout_->setSpacing(BASE_HEADER_SPACING);

    this->icon_ = new SvgButton(
        {
            .dark = ":/buttons/poll-darkMode.svg",
            .light = ":/buttons/poll-lightMode.svg",
        },
        this, QSize(2, 2));
    this->icon_->setFixedSize(BASE_ICON_SIZE, BASE_ICON_SIZE);
    this->icon_->setCursor(Qt::PointingHandCursor);
    auto *iconOpacity = new QGraphicsOpacityEffect(this);
    iconOpacity->setOpacity(0.55);
    this->icon_->setGraphicsEffect(iconOpacity);
    this->topLayout_->addWidget(this->icon_, 0, Qt::AlignBottom);

    this->metadataLabel_ = new QLabel(this);
    this->metadataLabel_->setContentsMargins(0, 0, 0, 0);
    this->metadataLabel_->setAlignment(Qt::AlignLeft | Qt::AlignBottom);
    this->metadataLabel_->setMinimumWidth(0);
    this->metadataLabel_->setSizePolicy(QSizePolicy::Expanding,
                                        QSizePolicy::Preferred);
    this->topLayout_->addWidget(this->metadataLabel_, 1, Qt::AlignBottom);

    this->timerLabel_ = new QLabel(this);
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
    this->closeButton_->setToolTip("Dismiss Poll");
    this->closeButton_->setFixedSize(BASE_ICON_SIZE, BASE_ICON_SIZE);
    this->topLayout_->addWidget(this->closeButton_, 0, Qt::AlignBottom);

    this->rootLayout_->addLayout(this->topLayout_);

    this->contentLayout_ = new QVBoxLayout();
    this->contentLayout_->setContentsMargins(BASE_CONTENT_LEFT_MARGIN, 0, 0, 0);
    this->contentLayout_->setSpacing(2);

    this->titleLabel_ = new QLabel(this);
    this->titleLabel_->setWordWrap(false);
    this->titleLabel_->setSizePolicy(QSizePolicy::Ignored,
                                     QSizePolicy::Preferred);
    this->contentLayout_->addWidget(this->titleLabel_);

    this->summaryLabel_ = new QLabel(this);
    this->summaryLabel_->setWordWrap(false);
    this->summaryLabel_->setSizePolicy(QSizePolicy::Ignored,
                                       QSizePolicy::Preferred);
    this->summaryLabel_->setTextFormat(Qt::RichText);
    this->contentLayout_->addWidget(this->summaryLabel_);

    this->rootLayout_->addLayout(this->contentLayout_);

    this->distributionBar_ = new QWidget(this);
    this->distributionBar_->setAttribute(Qt::WA_TransparentForMouseEvents);
    this->distributionBar_->setFixedHeight(BASE_BAR_HEIGHT);

    this->updateTimer_ = new QTimer(this);
    this->updateTimer_->setInterval(1000);
    QObject::connect(this->updateTimer_, &QTimer::timeout, this,
                     &PollBanner::updateTimer);

    QObject::connect(this->closeButton_, &Button::leftClicked, [this]() {
        if (this->poll_)
        {
            this->dismissedPollKey_ = this->dismissalKey(*this->poll_);
        }
        this->hide();
        this->dismissed.invoke();
    });
    QObject::connect(this->toggleButton_, &Button::leftClicked, [this]() {
        this->toggleBannerRequested.invoke();
    });
    QObject::connect(this->icon_, &Button::leftClicked, [this]() {
        this->openPollDialog();
    });

    getSettings()->pinBannerBackgroundColor.connect(
        [this](const QString &, auto) {
            this->update();
        },
        this->managedConnections_);

    getSettings()->pollBannerContentScale.connect(
        [this](const float &, auto) {
            this->scaleChangedEvent(this->scale());
            this->updateGeometry();
        },
        this->managedConnections_);

    this->hide();
}

void PollBanner::setPoll(const std::optional<TwitchChannel::PollEvent> &poll,
                         TwitchChannel *channel)
{
    if (!poll || !channel || !getSettings()->enablePolls)
    {
        this->poll_ = std::nullopt;
        this->twitchChannel_ = nullptr;
        this->metadataLabel_->clear();
        this->titleLabel_->clear();
        this->summaryLabel_->clear();
        this->timerLabel_->clear();
        this->distributionBar_->hide();
        this->updateTimer_->stop();
        this->anim_->stop();
        this->currentFractions_.clear();
        this->targetFractions_.clear();
        this->previousFractions_.clear();
        this->expiryRefreshQueued_ = false;
        this->hide();
        return;
    }

    const bool samePoll =
        this->poll_.has_value() && this->poll_->id == poll->id;
    if (!samePoll || !pollIsActive(*poll))
    {
        this->expiryRefreshQueued_ = false;
    }

    this->poll_ = poll;
    this->pollSnapshotAt_ = QDateTime::currentDateTimeUtc();
    this->twitchChannel_ = channel;

    std::vector<int> sortedIndices;
    sortedIndices.reserve(poll->choices.size());
    for (int i = 0; i < static_cast<int>(poll->choices.size()); ++i)
    {
        sortedIndices.push_back(i);
    }
    if (poll->totalVotes > 0)
    {
        std::stable_sort(sortedIndices.begin(), sortedIndices.end(),
                         [&poll](int a, int b) {
                             return poll->choices.at(size_t(a)).totalVotes >
                                    poll->choices.at(size_t(b)).totalVotes;
                         });
    }

    std::vector<double> newFractions;
    newFractions.reserve(sortedIndices.size());
    const double fallbackFraction =
        poll->choices.empty() ? 0.0 : 1.0 / poll->choices.size();
    for (const int index : sortedIndices)
    {
        const auto &choice = poll->choices.at(size_t(index));
        newFractions.push_back(poll->totalVotes > 0
                                   ? double(choice.totalVotes) /
                                         double(poll->totalVotes)
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
        const double progress =
            this->anim_->state() == QAbstractAnimation::Running
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

    if (pollIsActive(*poll))
    {
        this->updateTimer_->start();
    }
    else
    {
        this->updateTimer_->stop();
    }

    if (this->dismissedPollKey_ == this->dismissalKey(*poll))
    {
        this->hide();
        return;
    }

    const int autoDismiss = getSettings()->predictionAutoDismissSeconds;
    if (autoDismiss > 0 && !pollIsActive(*poll))
    {
        QTimer::singleShot(autoDismiss * 1000, this, [this] {
            if (this->poll_ && !pollIsActive(*this->poll_))
            {
                this->dismissedPollKey_ = this->dismissalKey(*this->poll_);
                this->hide();
                this->dismissed.invoke();
            }
        });
    }
}

bool PollBanner::hasPoll() const
{
    if (!this->poll_.has_value())
    {
        return false;
    }
    return this->dismissedPollKey_ != this->dismissalKey(*this->poll_);
}

void PollBanner::setToggleButtonVisible(bool visible)
{
    this->toggleButton_->setVisible(visible);
}

void PollBanner::scaleChangedEvent(float scale)
{
    BaseWidget::scaleChangedEvent(scale);

    const int iconSize = scaledInt(BASE_ICON_SIZE * scale);
    this->headerFontSize_ = scaledInt(BASE_HEADER_FONT_SIZE * scale);
    const float contentScale = pollBannerContentScale();
    this->titleFontSize_ =
        scaledInt(BASE_TITLE_FONT_SIZE * scale * contentScale);
    this->summaryFontSize_ =
        scaledInt(BASE_SUMMARY_FONT_SIZE * scale * contentScale);

    this->icon_->setFixedSize(iconSize, iconSize);
    this->closeButton_->setFixedSize(iconSize, iconSize);
    this->toggleButton_->setFixedSize(iconSize, iconSize);
    this->distributionBar_->setFixedHeight(
        scaledInt(BASE_BAR_HEIGHT * scale, 2));

    this->rootLayout_->setContentsMargins(
        0, scaledInt(BASE_TOP_MARGIN * scale, 0),
        scaledInt(BASE_RIGHT_MARGIN * scale, 0),
        scaledInt(BASE_BOTTOM_MARGIN * scale, 0));
    this->rootLayout_->setSpacing(scaledInt(BASE_SPACING * scale, 0));
    this->topLayout_->setContentsMargins(
        scaledInt(BASE_HEADER_LEFT_MARGIN * scale, 0), 0, 0, 0);
    this->topLayout_->setSpacing(scaledInt(BASE_HEADER_SPACING * scale, 0));
    const int textBottomInset =
        scaledInt(BASE_HEADER_TEXT_BOTTOM_INSET * scale, 0);
    this->metadataLabel_->setContentsMargins(0, 0, 0, textBottomInset);
    this->timerLabel_->setContentsMargins(0, 0, scaledInt(4 * scale, 0),
                                          textBottomInset);
    this->contentLayout_->setContentsMargins(
        scaledInt(BASE_CONTENT_LEFT_MARGIN * scale, 0), 0, 0, 0);
    this->contentLayout_->setSpacing(scaledInt(2 * scale));

    this->updateLabelStyles();
    if (this->poll_)
    {
        this->updateLayout();
        this->updateTimer();
    }
}

void PollBanner::themeChangedEvent()
{
    this->updateLabelStyles();
    this->icon_->setColor(this->theme->splits.header.text);
    this->toggleButton_->setColor(this->theme->splits.header.text);
    this->closeButton_->setColor(this->theme->splits.header.text);
    if (this->poll_)
    {
        this->updateLayout();
        this->updateTimer();
    }
    this->update();
}

void PollBanner::showEvent(QShowEvent *event)
{
    BaseWidget::showEvent(event);
    this->scaleChangedEvent(this->scale());
}

void PollBanner::paintEvent(QPaintEvent *)
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

    if (!this->poll_ || !this->distributionBar_->isVisible())
    {
        return;
    }

    const QRect barRect = this->distributionBar_->geometry();
    if (barRect.width() <= 0)
    {
        return;
    }

    QColor baseBarColor = this->theme->splits.header.border;
    baseBarColor.setAlpha(80);
    painter.fillRect(barRect, baseBarColor);

    painter.setRenderHint(QPainter::Antialiasing, false);

    int currentLeft = barRect.left();
    double cumulativeFraction = 0.0;
    const int outcomeCount = static_cast<int>(this->poll_->choices.size());
    const double fallbackFraction = outcomeCount > 0 ? 1.0 / outcomeCount : 0.0;

    std::vector<int> sortedIndices;
    sortedIndices.reserve(size_t(outcomeCount));
    for (int i = 0; i < outcomeCount; ++i)
    {
        sortedIndices.push_back(i);
    }
    if (this->poll_->totalVotes > 0)
    {
        std::stable_sort(
            sortedIndices.begin(), sortedIndices.end(), [this](int a, int b) {
                return this->poll_->choices.at(size_t(a)).totalVotes >
                       this->poll_->choices.at(size_t(b)).totalVotes;
            });
    }

    const bool votingEnded =
        !pollIsActive(*this->poll_) || this->remainingPollSeconds() <= 0;

    for (int orderedIndex = 0; orderedIndex < outcomeCount; ++orderedIndex)
    {
        const double progress =
            this->anim_->state() == QAbstractAnimation::Running
                ? this->anim_->currentValue().toDouble()
                : 1.0;
        const double oldFraction =
            orderedIndex < static_cast<int>(this->previousFractions_.size())
                ? this->previousFractions_.at(size_t(orderedIndex))
                : fallbackFraction;
        const double targetFraction =
            orderedIndex < static_cast<int>(this->targetFractions_.size())
                ? this->targetFractions_.at(size_t(orderedIndex))
                : fallbackFraction;
        const double fraction =
            oldFraction + (targetFraction - oldFraction) * progress;
        cumulativeFraction += fraction;

        const int nextLeft = (orderedIndex == outcomeCount - 1)
                                 ? (barRect.left() + barRect.width())
                                 : (barRect.left() + qRound(cumulativeFraction *
                                                            barRect.width()));

        const int sourceIndex = sortedIndices.at(size_t(orderedIndex));
        painter.fillRect(
            QRect(currentLeft, barRect.top(),
                  std::max(0, nextLeft - currentLeft), barRect.height()),
            pollToneForChoice(*this->poll_, sourceIndex, votingEnded));
        currentLeft = nextLeft;
    }
}

void PollBanner::resizeEvent(QResizeEvent *event)
{
    BaseWidget::resizeEvent(event);
    const int barH = this->distributionBar_->height();
    this->distributionBar_->setGeometry(0, this->height() - barH, this->width(),
                                        barH);
    if (this->poll_)
    {
        this->updateLayout();
    }
}

void PollBanner::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        this->openPollDialog();
        event->accept();
        return;
    }
    BaseWidget::mousePressEvent(event);
}

void PollBanner::updateTimer()
{
    if (!this->poll_)
    {
        return;
    }

    if (pollIsActive(*this->poll_))
    {
        const auto remainingSeconds = this->remainingPollSeconds();
        if (remainingSeconds <= 0 && !this->expiryRefreshQueued_)
        {
            this->expiryRefreshQueued_ = true;
            if (this->twitchChannel_ != nullptr)
            {
                this->twitchChannel_->refreshPollIfStale(true);
            }
        }

        if (remainingSeconds <= 0)
        {
            this->timerLabel_->setText("ENDED");
            this->timerLabel_->setStyleSheet(
                QString("font-size: %1px; color: #9ca3af; font-weight: 700;")
                    .arg(this->headerFontSize_));
            this->timerLabel_->show();
            this->updateTimer_->stop();
            this->updateLayout();
            return;
        }

        this->timerLabel_->setText(
            QString("%1 left").arg(formatRemainingPollTime(remainingSeconds)));
        this->timerLabel_->setStyleSheet(
            QString("font-size: %1px; color: #a855f7; font-weight: 700;")
                .arg(this->headerFontSize_));
        this->timerLabel_->show();
        return;
    }

    this->timerLabel_->setText("ENDED");
    this->timerLabel_->setStyleSheet(
        QString("font-size: %1px; color: #9ca3af; font-weight: 700;")
            .arg(this->headerFontSize_));
    this->timerLabel_->show();
}

qint64 PollBanner::remainingPollSeconds() const
{
    if (!this->poll_ || !pollIsActive(*this->poll_))
    {
        return 0;
    }

    if (this->poll_->endsAt.has_value() && this->poll_->endsAt->isValid())
    {
        return std::max<qint64>(0, QDateTime::currentDateTimeUtc().secsTo(
                                       this->poll_->endsAt->toUTC()));
    }

    if (this->poll_->createdAt.isValid() && this->poll_->durationSeconds > 0)
    {
        return std::max<qint64>(0, this->poll_->durationSeconds -
                                       this->poll_->createdAt.toUTC().secsTo(
                                           QDateTime::currentDateTimeUtc()));
    }

    const auto elapsedMs =
        this->pollSnapshotAt_.isValid()
            ? this->pollSnapshotAt_.msecsTo(QDateTime::currentDateTimeUtc())
            : 0;
    return std::max<qint64>(
        0, (this->poll_->remainingDurationMilliseconds - elapsedMs) / 1000);
}

void PollBanner::updateLayout()
{
    if (!this->poll_)
    {
        return;
    }

    QString metadata = "Poll";
    if (!this->poll_->createdByName.isEmpty())
    {
        metadata += QString(" by %1").arg(this->poll_->createdByName);
    }
    const auto age = formatPollAge(this->poll_->createdAt);
    if (!age.isEmpty())
    {
        metadata += QString::fromUtf8(" \xc2\xb7 %1").arg(age);
    }
    this->metadataLabel_->setText(metadata);

    this->titleLabel_->setText(
        QFontMetrics(this->titleLabel_->font())
            .elidedText(this->poll_->title, Qt::ElideRight,
                        std::max(16, this->titleLabel_->width() - 4)));

    if (this->poll_->choices.empty())
    {
        this->summaryLabel_->clear();
        this->distributionBar_->hide();
        this->update();
        return;
    }

    const bool votingEnded =
        !pollIsActive(*this->poll_) || this->remainingPollSeconds() <= 0;
    auto leaderIt = std::max_element(this->poll_->choices.begin(),
                                     this->poll_->choices.end(),
                                     [](const auto &a, const auto &b) {
                                         return a.totalVotes < b.totalVotes;
                                     });
    const int leaderIndex =
        static_cast<int>(std::distance(this->poll_->choices.begin(), leaderIt));
    const int percent =
        outcomePercent(leaderIt->totalVotes, this->poll_->totalVotes,
                       static_cast<int>(this->poll_->choices.size()));
    const auto tone = pollToneForChoice(*this->poll_, leaderIndex, votingEnded);
    auto dimTextColor = this->theme->splits.header.text;
    dimTextColor.setAlphaF(0.55F);

    if (this->poll_->totalVotes <= 0)
    {
        this->summaryLabel_->setText(
            QString("<span style=\"color:%1;\">%2</span>")
                .arg(dimTextColor.name(QColor::HexArgb),
                     votingEnded ? "Voting ended: no votes" : "No votes yet"));
        this->distributionBar_->show();
        this->update();
        return;
    }

    int leadingChoices = 0;
    QStringList tiedTitles;
    for (const auto &choice : this->poll_->choices)
    {
        if (choice.totalVotes == leaderIt->totalVotes)
        {
            ++leadingChoices;
            if (tiedTitles.size() < 2)
            {
                tiedTitles.push_back(choice.title);
            }
        }
    }

    const bool tiedEndedPoll = votingEnded && leadingChoices > 1;
    QString resultLabel = votingEnded ? "Winner:" : "Leading:";
    QString resultTitle = leaderIt->title;
    if (tiedEndedPoll)
    {
        resultLabel = "Tie:";
        resultTitle = tiedTitles.join(" / ");
        if (leadingChoices > tiedTitles.size())
        {
            resultTitle +=
                QString(" + %1").arg(leadingChoices - tiedTitles.size());
        }
    }

    this->summaryLabel_->setText(
        QString("<span style=\"color:%1;\">%2</span> "
                "<span style=\"color:%3;font-weight:700;\">%4</span> "
                "<span style=\"color:%1;\">%5% (%6 votes)</span>")
            .arg(dimTextColor.name(QColor::HexArgb), resultLabel, tone.name(),
                 resultTitle.toHtmlEscaped())
            .arg(percent)
            .arg(formatCompactVotes(leaderIt->totalVotes)));
    this->distributionBar_->show();
    this->update();
}

void PollBanner::updateLabelStyles()
{
    const QColor textColor = this->theme->splits.header.text;
    const auto dimColor = QString("rgba(%1, %2, %3, 0.55)")
                              .arg(textColor.red())
                              .arg(textColor.green())
                              .arg(textColor.blue());

    this->metadataLabel_->setStyleSheet(QString("font-size: %1px; color: %2;")
                                            .arg(this->headerFontSize_)
                                            .arg(dimColor));
    this->titleLabel_->setStyleSheet(
        QString("font-size: %1px; font-weight: bold; color: %2;")
            .arg(this->titleFontSize_)
            .arg(textColor.name()));
    this->summaryLabel_->setStyleSheet(
        QString("font-size: %1px;").arg(this->summaryFontSize_));
}

void PollBanner::openPollDialog()
{
    if (this->twitchChannel_ == nullptr)
    {
        return;
    }

    this->split_->setFocus(Qt::MouseFocusReason);
    PollDialog::showDialog(this->twitchChannel_, this->split_, this->poll_,
                           PollDialog::OpenMode::ShowPollResults);
}

QString PollBanner::dismissalKey(const TwitchChannel::PollEvent &poll) const
{
    return poll.id + ':' + poll.status;
}

}  // namespace chatterino

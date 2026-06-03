#include "widgets/dialogs/PollDialog.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "util/Helpers.hpp"
#include "widgets/buttons/Button.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/helper/Line.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QShowEvent>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>

namespace chatterino {

namespace {

constexpr int TITLE_LIMIT = 60;
constexpr int CHOICE_LIMIT = 25;
constexpr int MIN_CHOICES = 2;
constexpr int MAX_CHOICES = 5;
constexpr int CREATE_VISIBLE_RESPONSE_ROWS = 5;
constexpr QSize DEFAULT_DIALOG_SIZE(332, 360);
constexpr float CONTENT_SCALE_MULTIPLIER = 1.30F;
constexpr int HEADER_SEPARATOR_HEIGHT = 8;

struct DurationOption {
    const char *label;
    int seconds;
};

constexpr DurationOption DURATION_OPTIONS[] = {
    {"10 seconds", 10},  {"30 seconds", 30},   {"1 minute", 60},
    {"2 minutes", 120},  {"5 minutes", 300},   {"10 minutes", 600},
    {"15 minutes", 900}, {"30 minutes", 1800},
};

float contentScale(float scale)
{
    const float taper = std::clamp((scale - 1.0F) / 0.6F, 0.0F, 1.0F);
    const float multiplier =
        CONTENT_SCALE_MULTIPLIER - taper * 0.24F;  // MAX_CONTENT_SCALE_TAPER
    return scale * multiplier;
}

int scaledSeparatorHeight(float scale)
{
    return std::max(1, int(HEADER_SEPARATOR_HEIGHT * scale));
}

QString formatCompact(qint64 value)
{
    return value >= 0 ? formatCompactNumber(value) : QStringLiteral("...");
}

bool pollIsOpenForVoting(const TwitchChannel::PollEvent &poll)
{
    if (poll.status.compare("ACTIVE", Qt::CaseInsensitive) != 0)
    {
        return false;
    }

    return !poll.endsAt ||
           QDateTime::currentDateTimeUtc() < poll.endsAt.value();
}

class PollChoiceButton : public QPushButton
{
public:
    using QPushButton::QPushButton;

    void setFillProgress(double progress)
    {
        progress = std::clamp(progress, 0.0, 1.0);
        if (qFuzzyCompare(this->fillProgress_ + 1.0, progress + 1.0))
        {
            return;
        }

        this->fillProgress_ = progress;
        this->update();
    }

    double fillProgress() const
    {
        return this->fillProgress_;
    }

    void setFillColor(const QColor &color)
    {
        this->fillColor_ = color;
        this->update();
    }

    void setOutlineColor(const QColor &color)
    {
        this->outlineColor_ = color;
        this->update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QPushButton::paintEvent(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const auto inset = qreal(2);
        const QRectF bounds =
            QRectF(this->rect()).adjusted(inset, inset, -inset, -inset);
        if (this->fillProgress_ > 0.0 && this->fillColor_.isValid() &&
            bounds.width() > 0 && bounds.height() > 0)
        {
            QPainterPath clip;
            clip.addRoundedRect(bounds, 2.0, 2.0);
            painter.setClipPath(clip);

            QRectF fill = bounds;
            fill.setWidth(bounds.width() * this->fillProgress_);
            painter.fillRect(fill, this->fillColor_);
            painter.setClipping(false);
        }

        if (this->outlineColor_.isValid())
        {
            const QRectF outline =
                QRectF(this->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
            painter.setPen(QPen(this->outlineColor_, 1.25));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(outline, 2.0, 2.0);
        }
    }

private:
    double fillProgress_ = 0.0;
    QColor fillColor_;
    QColor outlineColor_;
};

QEasingCurve pollChoiceFillCurve()
{
    QEasingCurve curve(QEasingCurve::BezierSpline);
    curve.addCubicBezierSegment(QPointF(0.22, 0.61), QPointF(0.36, 1.0),
                                QPointF(1.0, 1.0));
    return curve;
}

std::optional<TwitchChannel::PollEvent> filterPollForOpenMode(
    std::optional<TwitchChannel::PollEvent> poll, PollDialog::OpenMode mode)
{
    if (mode == PollDialog::OpenMode::ShowPollResults)
    {
        return poll;
    }

    if (poll && !pollIsOpenForVoting(*poll))
    {
        return std::nullopt;
    }

    return poll;
}

bool shouldRefreshForOpenMode(
    const std::optional<TwitchChannel::PollEvent> &poll,
    PollDialog::OpenMode mode)
{
    if (mode != PollDialog::OpenMode::ShowPollResults)
    {
        return true;
    }

    return !poll || pollIsOpenForVoting(*poll);
}

QString formatPercent(int numerator, int denominator)
{
    if (denominator <= 0)
    {
        return "0%";
    }
    return QString("%1%").arg(
        int(std::round((numerator * 100.0) / denominator)));
}

QString moltorinoAuthRequiredMessage(const QString &action)
{
    return MoltorinoAuth::authRequiredMessage(action);
}

QString normalizeMoltorinoAuthError(const QString &action, const QString &error)
{
    return MoltorinoAuth::normalizeAuthError(action, error);
}

QString normalizePollCreateError(const QString &error)
{
    const auto authError = normalizeMoltorinoAuthError("creating polls", error);
    if (authError != error)
    {
        return authError;
    }

    const auto lowered = error.toLower();
    if (lowered.contains("affiliate") || lowered.contains("partner"))
    {
        return "This channel is not eligible for polls.";
    }
    return error;
}

QString normalizePollVoteError(const QString &error)
{
    const auto authError =
        normalizeMoltorinoAuthError("voting in polls", error);
    if (authError != error)
    {
        return authError;
    }

    if (error.trimmed().isEmpty())
    {
        return "Failed to vote in poll.";
    }
    return error;
}

QColor statusColor(const QString &status)
{
    if (status == "ACTIVE")
    {
        return QColor("#9147ff");
    }
    if (status == "COMPLETED")
    {
        return QColor("#22c55e");
    }
    if (status == "TERMINATED" || status == "ARCHIVED")
    {
        return QColor("#9ca3af");
    }
    return QColor("#64748b");
}

QString formatRemainingTime(int totalSeconds)
{
    const int clamped = std::max(0, totalSeconds);
    const int minutes = clamped / 60;
    const int seconds = clamped % 60;
    return QString("%1:%2").arg(minutes).arg(seconds, 2, 10, QChar('0'));
}

QString pollStatusLabel(const QString &status)
{
    if (status.compare("ACTIVE", Qt::CaseInsensitive) == 0)
    {
        return "active";
    }
    if (status.compare("COMPLETED", Qt::CaseInsensitive) == 0 ||
        status.compare("TERMINATED", Qt::CaseInsensitive) == 0 ||
        status.compare("ARCHIVED", Qt::CaseInsensitive) == 0)
    {
        return "ended";
    }
    return status.toLower();
}

}  // namespace

std::vector<QPointer<PollDialog>> PollDialog::activeDialogs_;

PollDialog::PollDialog(TwitchChannel *channel, QWidget *parent)
    : DraggablePopup(true, parent)
    , channel_(channel)
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setObjectName("PollDialog");
    this->setWindowTitle("Poll");
    this->setScaleIndependentSize(DEFAULT_DIALOG_SIZE);
    this->ensureDraft();
    this->currentPoll_ = *this->channel_->accessPoll();
    this->pollSnapshotAt_ = QDateTime::currentDateTimeUtc();

    auto *container = this->getLayoutContainer();
    container->setObjectName("PollDialogRoot");
    container->setMouseTracking(true);
    this->mainLayout_ = new QVBoxLayout(container);

    this->headerWidget_ = new QWidget(container);
    this->headerWidget_->setObjectName("PollDialogHeader");
    auto *headerLayout = new QHBoxLayout(this->headerWidget_);
    const int margin = std::max(3, int(5 * this->scale()));
    headerLayout->setContentsMargins(margin, 0, margin, 0);

    auto *headerTextLayout = new QVBoxLayout();
    headerTextLayout->setContentsMargins(0, 0, 0, 0);
    this->headerTitleLabel_ = new QLabel("Poll", this->headerWidget_);
    this->headerTitleLabel_->setObjectName("PollHeaderTitle");
    this->headerSubtitleLabel_ = new QLabel("", this->headerWidget_);
    this->headerSubtitleLabel_->setObjectName("PollHeaderSubtitle");
    headerTextLayout->addWidget(this->headerTitleLabel_);
    headerTextLayout->addWidget(this->headerSubtitleLabel_);
    headerLayout->addLayout(headerTextLayout, 1);

    this->pinButton_ = this->createPinButton();
    this->pinButton_->setToolTip("Pin Poll Popup");
    this->pinButton_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    if (!getSettings()->predictionCloseOnFocusLoss)
    {
        this->ensurePinned();
    }
    headerLayout->addWidget(this->pinButton_);

    this->closeButton_ = new SvgButton(
        {
            .dark = ":/buttons/cancel.svg",
            .light = ":/buttons/cancelDark.svg",
        },
        this, QSize{3, 3});
    this->closeButton_->setScaleIndependentSize(18, 18);
    this->closeButton_->setToolTip("Close");
    this->closeButton_->setCursor(Qt::PointingHandCursor);
    QObject::connect(this->closeButton_, &Button::leftClicked, this,
                     &QWidget::close);
    headerLayout->addWidget(this->closeButton_);

    this->mainLayout_->addWidget(this->headerWidget_);

    auto *separator = new Line(false);
    separator->setObjectName("PollDialogSeparator");
    separator->setFixedHeight(scaledSeparatorHeight(this->scale()));
    this->mainLayout_->addWidget(separator);

    this->scrollArea_ = new QScrollArea(container);
    this->scrollArea_->setObjectName("PollScrollArea");
    this->scrollArea_->setFrameShape(QFrame::NoFrame);
    this->scrollArea_->setWidgetResizable(true);
    this->scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->scrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->scrollArea_->viewport()->installEventFilter(this);
    this->mainLayout_->addWidget(this->scrollArea_, 1);

    this->activeWidget_ = new QWidget();
    this->activeWidget_->setObjectName("PollDialogContent");
    this->activeWidget_->setMinimumHeight(0);
    this->activeWidget_->setSizePolicy(QSizePolicy::Preferred,
                                       QSizePolicy::Expanding);
    this->scrollArea_->setWidget(this->activeWidget_);

    this->countdownTimer_ = new QTimer(this);
    this->countdownTimer_->setInterval(1000);
    QObject::connect(this->countdownTimer_, &QTimer::timeout, this, [this] {
        this->updateCountdownLabel();
    });

    this->managedConnections_.emplace_back(
        this->channel_->pollChanged.connect([this] {
            this->setPoll(filterPollForOpenMode(
                *this->channel_->accessPoll(),
                this->showInactivePollResults_ ? OpenMode::ShowPollResults
                                               : OpenMode::CreateOrActivePoll));
        }));
    this->managedConnections_.emplace_back(
        this->channel_->channelPointsChanged.connect([this] {
            if (this->currentPoll_ &&
                this->currentPoll_->channelPointsVotingEnabled &&
                !this->isBroadcasterView())
            {
                this->updateUi();
            }
        }));

    this->refreshHeader();
    this->refreshStyle();
    this->updateUi();
}

void PollDialog::showDialog(TwitchChannel *channel, QWidget *parent,
                            const std::optional<TwitchChannel::PollEvent> &poll,
                            OpenMode mode)
{
    auto pollToShow = poll;
    if (!pollToShow)
    {
        pollToShow = *channel->accessPoll();
    }
    pollToShow = filterPollForOpenMode(std::move(pollToShow), mode);

    for (auto it = activeDialogs_.begin(); it != activeDialogs_.end();)
    {
        if (it->isNull())
        {
            it = activeDialogs_.erase(it);
            continue;
        }
        if ((*it)->channel_ == channel)
        {
            (*it)->showInactivePollResults_ = mode == OpenMode::ShowPollResults;
            (*it)->setPoll(pollToShow);
            (*it)->raise();
            (*it)->activateWindow();
            if (shouldRefreshForOpenMode(pollToShow, mode))
            {
                channel->refreshPollIfStale();
            }
            return;
        }
        ++it;
    }

    auto *dialog = new PollDialog(channel, parent);
    dialog->showInactivePollResults_ = mode == OpenMode::ShowPollResults;
    dialog->setPoll(pollToShow);
    activeDialogs_.push_back(dialog);
    if (shouldRefreshForOpenMode(pollToShow, mode))
    {
        channel->refreshPollIfStale(true);
    }

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
}

void PollDialog::setPoll(const std::optional<TwitchChannel::PollEvent> &poll)
{
    const bool samePoll = this->currentPoll_.has_value() && poll.has_value() &&
                          this->currentPoll_->id == poll->id;
    if (!samePoll || !poll.has_value() ||
        poll->status.compare("ACTIVE", Qt::CaseInsensitive) != 0)
    {
        this->pollExpiryRefreshQueued_ = false;
    }
    if (!samePoll)
    {
        this->pollChoiceFillProgress_.clear();
        this->suppressSelectedChoiceOutline_ = false;
    }

    this->currentPoll_ = poll;
    this->pollSnapshotAt_ = QDateTime::currentDateTimeUtc();
    if (!this->currentPoll_)
    {
        this->selectedChoiceId_.clear();
        this->suppressSelectedChoiceOutline_ = false;
    }
    else
    {
        bool found = false;
        for (const auto &choice : this->currentPoll_->choices)
        {
            if (choice.id == this->selectedChoiceId_)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            this->selectedChoiceId_.clear();
            this->suppressSelectedChoiceOutline_ = false;
        }
    }
    this->refreshHeader();
    this->updateUi();
    this->updateCountdownTimer();
}

void PollDialog::themeChangedEvent()
{
    this->refreshStyle();
}

void PollDialog::scaleChangedEvent(float)
{
    this->refreshStyle();
    this->updateUi();
}

void PollDialog::showEvent(QShowEvent *event)
{
    DraggablePopup::showEvent(event);
    this->updateCountdownLabel();
    if (this->currentPoll_ && this->currentPoll_->channelPointsVotingEnabled &&
        !this->isBroadcasterView())
    {
        this->channel_->refreshChannelPointsIfStale();
    }
}

bool PollDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (this->scrollArea_ != nullptr &&
        watched == this->scrollArea_->viewport() &&
        event->type() == QEvent::Resize)
    {
        this->fitCreateOptionsList();
        this->fitVoteOptionsList();
        QTimer::singleShot(0, this, [this] {
            this->fitCreateOptionsList();
            this->fitVoteOptionsList();
        });
    }

    return DraggablePopup::eventFilter(watched, event);
}

void PollDialog::refreshHeader()
{
    if (!this->currentPoll_)
    {
        this->headerTitleLabel_->setText("Start a Poll");
        this->headerSubtitleLabel_->setText('#' + this->channel_->getName());
        return;
    }

    this->headerTitleLabel_->setText("Poll");
    const bool votingEnded = this->currentPoll_->status.compare(
                                 "ACTIVE", Qt::CaseInsensitive) == 0 &&
                             this->remainingPollSeconds() <= 0;
    const auto status =
        votingEnded ? QString("COMPLETED") : this->currentPoll_->status;
    this->headerSubtitleLabel_->setText(
        QString("<span style=\"font-weight:400;\">#%1</span>"
                " <span style=\"font-weight:400;\">•</span> "
                "<span style=\"font-weight:700; color:%2;\">%3</span>")
            .arg(this->channel_->getName().toHtmlEscaped(),
                 statusColor(status).name(),
                 pollStatusLabel(status).toHtmlEscaped()));
}

void PollDialog::ensureDraft()
{
    while (this->draftChoices_.size() < MAX_CHOICES)
    {
        this->draftChoices_.push_back("");
    }
}

int PollDialog::currentUserTotalVotes() const
{
    if (!this->currentPoll_)
    {
        return 0;
    }

    int total = 0;
    for (const auto &selfVote : this->currentPoll_->selfVotes)
    {
        total += selfVote.freeVotes + selfVote.channelPointsVotes;
    }
    return total;
}

int PollDialog::remainingPollSeconds() const
{
    if (!this->currentPoll_ || this->currentPoll_->status != "ACTIVE")
    {
        return 0;
    }

    if (this->currentPoll_->endsAt && this->currentPoll_->endsAt->isValid())
    {
        return std::max(0, int(QDateTime::currentDateTimeUtc().secsTo(
                               this->currentPoll_->endsAt->toUTC())));
    }

    if (this->currentPoll_->createdAt.isValid() &&
        this->currentPoll_->durationSeconds > 0)
    {
        return std::max(0, this->currentPoll_->durationSeconds -
                               int(this->currentPoll_->createdAt.toUTC().secsTo(
                                   QDateTime::currentDateTimeUtc())));
    }

    const auto elapsedMs =
        this->pollSnapshotAt_.isValid()
            ? this->pollSnapshotAt_.msecsTo(QDateTime::currentDateTimeUtc())
            : 0;
    return std::max(
        0, int((this->currentPoll_->remainingDurationMilliseconds - elapsedMs) /
               1000));
}

void PollDialog::updateCountdownLabel()
{
    if (this->countdownLabel_ != nullptr && this->currentPoll_)
    {
        const auto remaining = this->remainingPollSeconds();
        this->countdownLabel_->setText(
            this->currentPoll_->status == "ACTIVE" && remaining > 0
                ? QString("Ending in %1").arg(formatRemainingTime(remaining))
                : QString("Voting ended"));

        if (this->currentPoll_->status == "ACTIVE" && remaining <= 0 &&
            !this->pollExpiryRefreshQueued_)
        {
            this->pollExpiryRefreshQueued_ = true;
            this->refreshHeader();
            this->channel_->refreshPollIfStale(true);
            QTimer::singleShot(0, this, [this] {
                this->updateUi();
            });
        }
    }

    this->updateCountdownTimer();
}

void PollDialog::updateCountdownTimer()
{
    if (this->countdownTimer_ == nullptr)
    {
        return;
    }

    const bool shouldRun = this->currentPoll_ &&
                           this->currentPoll_->status == "ACTIVE" &&
                           this->remainingPollSeconds() > 0;
    if (shouldRun && !this->countdownTimer_->isActive())
    {
        this->countdownTimer_->start();
    }
    else if (!shouldRun && this->countdownTimer_->isActive())
    {
        this->countdownTimer_->stop();
    }
}

bool PollDialog::isBroadcasterView() const
{
    return this->channel_ != nullptr && this->channel_->isBroadcaster();
}

void PollDialog::updateUi()
{
    this->updateGuard_++;
    this->setUpdatesEnabled(false);

    this->refreshHeader();

    const bool createMode = !this->currentPoll_.has_value();
    if (this->scrollArea_ != nullptr)
    {
        this->scrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        this->scrollArea_->setSizePolicy(QSizePolicy::Preferred,
                                         QSizePolicy::Expanding);
        this->scrollArea_->setMinimumHeight(0);
        this->scrollArea_->setMaximumHeight(QWIDGETSIZE_MAX);
    }

    if (auto *oldLayout = this->activeWidget_->layout())
    {
        delete oldLayout;
    }
    qDeleteAll(this->activeWidget_->findChildren<QWidget *>(
        QString(), Qt::FindDirectChildrenOnly));

    this->outcomesScrollArea_ = nullptr;
    this->createOutcomesPanel_ = nullptr;
    this->voteQuestionCard_ = nullptr;
    this->voteOutcomesPanel_ = nullptr;
    this->countdownLabel_ = nullptr;

    const float rawScale = this->scale();
    auto *layout = new QVBoxLayout(this->activeWidget_);
    const int margin = std::max(3, int(5 * rawScale));
    const int topMargin = margin;
    const int bottomMargin = 0;

    layout->setContentsMargins(margin, topMargin, margin, bottomMargin);
    layout->setSpacing(std::max(2, int(3 * rawScale)));

    if (this->bottomWidget_ != nullptr)
    {
        this->bottomWidget_->deleteLater();
        this->bottomWidget_ = nullptr;
    }

    if (createMode)
    {
        if (this->channel_->isMod() || this->channel_->isBroadcaster())
        {
            this->buildCreateUi();
        }
        else
        {
            auto *card = new QFrame(this->activeWidget_);
            card->setObjectName("PollCard");
            auto *cardLayout = new QVBoxLayout(card);
            auto *label = new QLabel("No active poll in this channel.", card);
            label->setAlignment(Qt::AlignCenter);
            label->setWordWrap(true);
            cardLayout->addWidget(label);
            layout->addWidget(card);
            layout->addStretch(1);
        }
    }
    else
    {
        this->buildVoteUi();
    }

    this->refreshStyle();
    this->applySizeConstraints(true);
    this->fitCreateOptionsList();
    this->fitVoteOptionsList();
    this->applySizeConstraints(true);

    QTimer::singleShot(0, this, [this] {
        this->applySizeConstraints(true);
        this->fitCreateOptionsList();
        this->fitVoteOptionsList();
        this->applySizeConstraints(true);
        if (--this->updateGuard_ <= 0)
        {
            this->updateGuard_ = 0;
            this->setUpdatesEnabled(true);
        }
    });
}

void PollDialog::buildCreateUi()
{
    this->ensureDraft();

    auto *layout = static_cast<QVBoxLayout *>(this->activeWidget_->layout());
    const float rawScale = this->scale();
    const float effectiveScale = contentScale(rawScale);
    const auto uiFont =
        getApp()->getFonts()->getFont(FontStyle::UiMedium, effectiveScale);
    const auto buttonFont =
        getApp()->getFonts()->getFont(FontStyle::UiMediumBold, effectiveScale);
    const QFontMetrics uiMetrics(uiFont);
    const QFontMetrics buttonMetrics(buttonFont);
    const int rowSpacing = std::max(1, int(3 * effectiveScale));
    const int sectionSpacing = std::max(1, int(4 * effectiveScale));
    const int responseListPad = std::max(1, int(2 * effectiveScale));
    const int accentWidth = std::max(1, int(2 * effectiveScale));
    const int visibleResponseRows =
        std::min(rawScale <= 0.65F   ? 3
                 : rawScale <= 0.85F ? 4
                                     : CREATE_VISIBLE_RESPONSE_ROWS,
                 MAX_CHOICES);
    const int inputHeight = std::max(
        10,
        std::max(int(20 * effectiveScale),
                 uiMetrics.height() + std::max(1, int(2 * effectiveScale))));
    const int compactControlHeight =
        std::max(10, std::max(int(20 * effectiveScale),
                              buttonMetrics.height() +
                                  std::max(1, int(2 * effectiveScale))));
    const int accentHeight =
        std::max(8, inputHeight - std::max(2, int(rawScale)));
    const int panelHeight = visibleResponseRows * inputHeight +
                            std::max(0, visibleResponseRows - 1) * rowSpacing +
                            responseListPad * 2;

    int filledChoices = 0;
    for (const auto &choice : this->draftChoices_)
    {
        if (!choice.trimmed().isEmpty())
        {
            ++filledChoices;
        }
    }

    auto *titleInput = new QLineEdit(this->activeWidget_);
    titleInput->setObjectName("PollTitleInput");
    titleInput->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    titleInput->setMaxLength(TITLE_LIMIT);
    titleInput->setPlaceholderText(
        QString("Poll title (%1 chars)").arg(TITLE_LIMIT));
    titleInput->setText(this->draftTitle_);
    titleInput->setFont(uiFont);
    titleInput->setFixedHeight(inputHeight);
    QObject::connect(titleInput, &QLineEdit::textChanged, this,
                     [this](const QString &text) {
                         this->draftTitle_ = text.left(TITLE_LIMIT);
                     });
    layout->addWidget(titleInput);

    {
        auto *headerRow = new QHBoxLayout();
        headerRow->setContentsMargins(0, 0, 0, 0);
        headerRow->setSpacing(rowSpacing);
        auto *label = new QLabel("Responses", this->activeWidget_);
        label->setObjectName("PollSectionTitle");
        label->setFont(buttonFont);
        headerRow->addWidget(label);
        headerRow->addStretch(1);
        layout->addLayout(headerRow);
    }

    auto *choicesPanel = new QWidget(this->activeWidget_);
    this->createOutcomesPanel_ = choicesPanel;
    choicesPanel->setObjectName("PollOutcomesPanel");
    choicesPanel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    choicesPanel->setProperty("pollWantedHeight", panelHeight);
    choicesPanel->setMinimumHeight(panelHeight);
    choicesPanel->setMaximumHeight(QWIDGETSIZE_MAX);
    auto *choicesPanelLayout = new QVBoxLayout(choicesPanel);
    choicesPanelLayout->setContentsMargins(0, 0, 0, 0);
    choicesPanelLayout->setSpacing(0);

    this->outcomesScrollArea_ = new QScrollArea(choicesPanel);
    this->outcomesScrollArea_->setObjectName("PollOutcomesScrollArea");
    this->outcomesScrollArea_->setFrameShape(QFrame::NoFrame);
    this->outcomesScrollArea_->setWidgetResizable(true);
    this->outcomesScrollArea_->setFocusPolicy(Qt::NoFocus);
    this->outcomesScrollArea_->setHorizontalScrollBarPolicy(
        Qt::ScrollBarAlwaysOff);
    this->outcomesScrollArea_->setVerticalScrollBarPolicy(
        Qt::ScrollBarAsNeeded);
    this->outcomesScrollArea_->setSizePolicy(QSizePolicy::Expanding,
                                             QSizePolicy::Expanding);
    this->outcomesScrollArea_->setProperty("pollWantedHeight", panelHeight);
    choicesPanelLayout->addWidget(this->outcomesScrollArea_);

    auto *choicesWidget = new QWidget();
    choicesWidget->setObjectName("PollOutcomesContent");
    choicesWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    auto *choicesLayout = new QVBoxLayout(choicesWidget);
    choicesLayout->setContentsMargins(responseListPad, responseListPad,
                                      responseListPad, responseListPad);
    choicesLayout->setSpacing(rowSpacing);
    choicesLayout->setSizeConstraint(QLayout::SetMinimumSize);

    for (int i = 0; i < MAX_CHOICES; ++i)
    {
        auto *rowWidget = new QWidget(choicesWidget);
        rowWidget->setFixedHeight(inputHeight);
        auto *rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(rowSpacing);

        auto *accent = new QWidget(rowWidget);
        accent->setFixedWidth(accentWidth);
        accent->setFixedHeight(accentHeight);
        accent->setStyleSheet(QString("background:%1; border-radius:%2px;")
                                  .arg(QColor("#22c55e").name())
                                  .arg(std::max(1, accentWidth / 2)));
        rowLayout->addWidget(accent, 0, Qt::AlignVCenter);

        auto *input = new QLineEdit(this->draftChoices_.value(i), rowWidget);
        input->setObjectName("PollOptionInput");
        input->setMaxLength(CHOICE_LIMIT);
        input->setPlaceholderText(i < 2 ? QString("Response %1").arg(i + 1)
                                        : QString("Response %1").arg(i + 1));
        input->setFont(uiFont);
        input->setFixedHeight(inputHeight);
        QObject::connect(input, &QLineEdit::textChanged, this,
                         [this, i](const QString &text) {
                             this->draftChoices_[i] = text.left(CHOICE_LIMIT);
                         });
        rowLayout->addWidget(input, 1);

        choicesLayout->addWidget(rowWidget);
    }

    choicesLayout->addStretch(1);

    this->outcomesScrollArea_->setWidget(choicesWidget);
    layout->addWidget(choicesPanel, 1);

    this->bottomWidget_ = new QWidget(this);
    this->bottomWidget_->setObjectName("PollBottomBar");
    this->bottomWidget_->setFont(uiFont);
    this->bottomWidget_->setSizePolicy(QSizePolicy::Preferred,
                                       QSizePolicy::Fixed);
    auto *bottomLayout = new QVBoxLayout(this->bottomWidget_);
    const int pad = std::max(1, int(3 * effectiveScale));
    bottomLayout->setContentsMargins(pad, pad, pad, pad);
    bottomLayout->setSpacing(sectionSpacing);

    auto *additionalWidget = new QWidget(this->bottomWidget_);
    additionalWidget->setFixedHeight(compactControlHeight +
                                     std::max(2, sectionSpacing));
    auto *additionalRow = new QHBoxLayout(additionalWidget);
    additionalRow->setContentsMargins(0, 0, 0, 0);
    additionalRow->setSpacing(rowSpacing);
    additionalRow->setAlignment(Qt::AlignVCenter);
    auto *additionalVotes =
        new QCheckBox("Allow Additional Votes", this->bottomWidget_);
    additionalVotes->setObjectName("PollCheckBox");
    additionalVotes->setChecked(this->draftEnableAdditionalVotes_);
    additionalVotes->setFont(uiFont);
    additionalVotes->setFixedHeight(compactControlHeight);
    QObject::connect(additionalVotes, &QCheckBox::toggled, this,
                     [this](bool checked) {
                         this->draftEnableAdditionalVotes_ = checked;
                         this->updateUi();
                     });
    additionalRow->addWidget(additionalVotes, 0, Qt::AlignVCenter);
    additionalRow->addStretch(1);

    auto *pointsEdit = new QLineEdit(QString::number(this->draftPointsPerVote_),
                                     this->bottomWidget_);
    pointsEdit->setObjectName("PollPointsInput");
    pointsEdit->setValidator(new QIntValidator(1, 1000000, pointsEdit));
    pointsEdit->setFont(uiFont);
    pointsEdit->setFixedHeight(compactControlHeight);
    pointsEdit->setFixedWidth(std::max(52, int(54 * effectiveScale)));
    pointsEdit->setEnabled(this->draftEnableAdditionalVotes_);
    QObject::connect(
        pointsEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
            bool ok = false;
            const int value = text.toInt(&ok);
            this->draftPointsPerVote_ = ok ? std::max(1, value) : 10;
        });
    additionalRow->addWidget(pointsEdit, 0, Qt::AlignVCenter);

    auto *pointsLabel = new QLabel("Points", this->bottomWidget_);
    pointsLabel->setObjectName("PollInfoLabel");
    pointsLabel->setFont(uiFont);
    pointsLabel->setEnabled(this->draftEnableAdditionalVotes_);
    additionalRow->addWidget(pointsLabel, 0, Qt::AlignVCenter);
    bottomLayout->addWidget(additionalWidget);

    {
        auto *durationRow = new QHBoxLayout();
        durationRow->setSpacing(rowSpacing);

        auto *durationLabel = new QLabel("Duration", this->bottomWidget_);
        durationLabel->setObjectName("PollSectionTitle");
        durationLabel->setFont(buttonFont);
        durationRow->addWidget(durationLabel);

        auto *durationCombo = new QComboBox(this->bottomWidget_);
        durationCombo->setObjectName("PollCreateDurationCombo");
        durationCombo->setFont(uiFont);
        durationCombo->setFixedHeight(compactControlHeight);
        durationCombo->setSizePolicy(QSizePolicy::Expanding,
                                     QSizePolicy::Fixed);
        int currentIndex = 0;
        for (int i = 0; i < static_cast<int>(std::size(DURATION_OPTIONS)); ++i)
        {
            durationCombo->addItem(DURATION_OPTIONS[i].label,
                                   DURATION_OPTIONS[i].seconds);
            if (DURATION_OPTIONS[i].seconds == this->draftDurationSeconds_)
            {
                currentIndex = i;
            }
        }
        durationCombo->setCurrentIndex(currentIndex);
        QObject::connect(durationCombo,
                         qOverload<int>(&QComboBox::currentIndexChanged), this,
                         [this, durationCombo](int index) {
                             if (index >= 0)
                             {
                                 this->draftDurationSeconds_ =
                                     durationCombo->itemData(index).toInt();
                             }
                         });
        durationRow->addWidget(durationCombo, 1);
        bottomLayout->addLayout(durationRow);
    }

    auto *startButton =
        new QPushButton(this->actionInFlight_ ? "Starting..." : "Start Poll",
                        this->bottomWidget_);
    startButton->setObjectName("PollCreateStartButton");
    startButton->setFont(buttonFont);
    startButton->setFixedHeight(compactControlHeight);
    startButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    startButton->setEnabled(!this->actionInFlight_);
    QObject::connect(startButton, &QPushButton::clicked, this, [this] {
        this->createPoll();
    });
    bottomLayout->addWidget(startButton);
    this->mainLayout_->addWidget(this->bottomWidget_);

    QTimer::singleShot(0, this, [this] {
        this->fitCreateOptionsList();
    });
}

void PollDialog::buildVoteUi()
{
    auto *layout = static_cast<QVBoxLayout *>(this->activeWidget_->layout());
    const auto &poll = *this->currentPoll_;
    const bool broadcasterView = this->isBroadcasterView();
    const float rawScale = this->scale();
    const float effectiveScale = contentScale(rawScale);
    const auto uiFont =
        getApp()->getFonts()->getFont(FontStyle::UiMedium, effectiveScale);
    const auto buttonFont =
        getApp()->getFonts()->getFont(FontStyle::UiMediumBold, effectiveScale);
    const auto titleFont = getApp()->getFonts()->getFont(
        FontStyle::UiMediumBold, effectiveScale * 1.18F);
    const auto compactMetaFont = getApp()->getFonts()->getFont(
        FontStyle::UiMedium, effectiveScale * 0.82F);
    const auto compactPctFont = getApp()->getFonts()->getFont(
        FontStyle::UiMediumBold, effectiveScale * 1.00F);
    const QFontMetrics uiMetrics(uiFont);
    const int rowSpacing = std::max(1, int(3 * effectiveScale));
    const int sectionSpacing = std::max(1, int(2 * rawScale));
    const int sectionPad = std::max(1, int(4 * rawScale));
    const int accentWidth = std::max(2, int(3 * effectiveScale));
    const int accentTextSpacing = std::max(4, int(5 * effectiveScale));
    const int textOpticalLift = std::max(1, int(std::round(rawScale)));
    const int outcomeRowHeight = std::max(
        10,
        std::max(int(26 * effectiveScale),
                 uiMetrics.height() + std::max(6, int(7 * effectiveScale))));
    const int accentHeight =
        std::max(12, outcomeRowHeight - std::max(6, int(8 * effectiveScale)));
    const int visibleRows = MAX_CHOICES;
    const int listHeight = visibleRows * outcomeRowHeight +
                           std::max(0, visibleRows - 1) * rowSpacing +
                           sectionPad * 2;

    auto *questionCard = new QWidget(this->activeWidget_);
    this->voteQuestionCard_ = questionCard;
    questionCard->setObjectName("PollQuestionCard");
    questionCard->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *questionLayout = new QVBoxLayout(questionCard);
    questionLayout->setContentsMargins(
        sectionPad * 2, std::max(2, int(5 * effectiveScale)), sectionPad * 2,
        std::max(1, int(3 * effectiveScale)));
    questionLayout->setSpacing(std::max(1, int(2 * effectiveScale)));

    auto *titleLabel = new QLabel(poll.title, questionCard);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setWordWrap(true);
    titleLabel->setFont(titleFont);
    titleLabel->setObjectName("PollCurrentTitle");
    questionLayout->addWidget(titleLabel);

    auto *timerLabel = new QLabel("Voting ended", questionCard);
    this->countdownLabel_ = timerLabel;
    timerLabel->setAlignment(Qt::AlignCenter);
    timerLabel->setObjectName("PollCountdown");
    timerLabel->setFont(uiFont);
    questionLayout->addWidget(timerLabel);
    this->updateCountdownLabel();
    layout->addWidget(questionCard);
    layout->addSpacing(sectionSpacing);

    auto *outcomesPanel = new QWidget(this->activeWidget_);
    this->voteOutcomesPanel_ = outcomesPanel;
    outcomesPanel->setObjectName("PollOutcomesPanel");
    outcomesPanel->setSizePolicy(QSizePolicy::Preferred,
                                 QSizePolicy::Expanding);
    auto *outcomesPanelLayout = new QVBoxLayout(outcomesPanel);
    outcomesPanelLayout->setContentsMargins(0, 0, 0, 0);
    outcomesPanelLayout->setSpacing(0);

    this->outcomesScrollArea_ = new QScrollArea(outcomesPanel);
    this->outcomesScrollArea_->setObjectName("PollOutcomesScrollArea");
    this->outcomesScrollArea_->setFrameShape(QFrame::NoFrame);
    this->outcomesScrollArea_->setWidgetResizable(true);
    this->outcomesScrollArea_->setHorizontalScrollBarPolicy(
        Qt::ScrollBarAlwaysOff);
    this->outcomesScrollArea_->setVerticalScrollBarPolicy(
        Qt::ScrollBarAsNeeded);
    this->outcomesScrollArea_->setSizePolicy(QSizePolicy::Expanding,
                                             QSizePolicy::Expanding);
    this->outcomesScrollArea_->setProperty("pollWantedHeight", listHeight);
    outcomesPanel->setProperty("pollWantedHeight", listHeight);
    outcomesPanel->setMinimumHeight(listHeight);
    outcomesPanel->setMaximumHeight(QWIDGETSIZE_MAX);
    outcomesPanelLayout->addWidget(this->outcomesScrollArea_);

    auto *outcomesWidget = new QWidget();
    outcomesWidget->setObjectName("PollOutcomesContent");
    outcomesWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    auto *outcomesLayout = new QVBoxLayout(outcomesWidget);
    outcomesLayout->setContentsMargins(sectionPad, sectionPad, sectionPad,
                                       sectionPad);
    outcomesLayout->setSpacing(rowSpacing);
    outcomesLayout->setSizeConstraint(QLayout::SetMinimumSize);

    const QLocale locale;
    int maxChoiceVotes = 0;
    for (const auto &choice : poll.choices)
    {
        maxChoiceVotes = std::max(maxChoiceVotes, choice.totalVotes);
    }
    const QFontMetrics metaMetrics(compactMetaFont);
    const QFontMetrics pctMetrics(compactPctFont);
    const int votesValueWidth = metaMetrics.horizontalAdvance(
        locale.toString(std::max(99, maxChoiceVotes)));
    const int pctValueWidth = pctMetrics.horizontalAdvance("100%");
    const auto rowTextColor = this->theme->window.text;
    auto rowMutedTextColor = rowTextColor;
    rowMutedTextColor.setAlpha(175);
    auto neutralPillColor = rowTextColor;
    neutralPillColor.setAlpha(this->theme->isLightTheme() ? 80 : 95);
    auto resultFillColor = rowTextColor;
    resultFillColor.setAlpha(this->theme->isLightTheme() ? 22 : 34);
    auto endedWinnerFillColor = QColor("#22c55e");
    endedWinnerFillColor.setAlpha(this->theme->isLightTheme() ? 50 : 80);
    auto endedTrailingFillColor = rowTextColor;
    endedTrailingFillColor.setAlpha(this->theme->isLightTheme() ? 22 : 34);
    const bool votingEnded =
        poll.status.compare("ACTIVE", Qt::CaseInsensitive) != 0 ||
        this->remainingPollSeconds() <= 0;

    for (int i = 0; i < static_cast<int>(poll.choices.size()); ++i)
    {
        const auto &choice = poll.choices.at(size_t(i));
        const bool isWinner =
            poll.totalVotes > 0 && choice.totalVotes == maxChoiceVotes;

        const int pct = poll.totalVotes > 0
                            ? static_cast<int>((choice.totalVotes * 100.0) /
                                               poll.totalVotes)
                            : 0;

        bool hasVotedForThis = false;
        if (!broadcasterView)
        {
            for (const auto &selfVote : poll.selfVotes)
            {
                if (selfVote.choiceId == choice.id)
                {
                    hasVotedForThis = true;
                    break;
                }
            }
        }

        const double targetFillProgress =
            poll.totalVotes > 0
                ? std::clamp(choice.totalVotes / double(poll.totalVotes), 0.0,
                             1.0)
                : 0.0;

        auto *rowButton = new PollChoiceButton(outcomesWidget);
        rowButton->setObjectName("PollChoiceButton");
        rowButton->setCheckable(!broadcasterView);
        rowButton->setChecked(!broadcasterView &&
                              choice.id == this->selectedChoiceId_);
        rowButton->setCursor(broadcasterView ? Qt::ArrowCursor
                                             : Qt::PointingHandCursor);
        rowButton->setFixedHeight(outcomeRowHeight);
        rowButton->setFillColor(
            votingEnded
                ? (isWinner ? endedWinnerFillColor : endedTrailingFillColor)
                : resultFillColor);
        const double startFillProgress =
            this->pollChoiceFillProgress_.value(choice.id, targetFillProgress);
        rowButton->setFillProgress(startFillProgress);
        this->pollChoiceFillProgress_.insert(choice.id, startFillProgress);

        QColor accentColor = neutralPillColor;
        if (hasVotedForThis && isWinner)
        {
            accentColor = QColor("#22c55e");
        }
        else if (hasVotedForThis)
        {
            accentColor = rowTextColor;
        }
        else if (isWinner)
        {
            accentColor = QColor("#22c55e");
        }

        const bool isSelectedChoice =
            !broadcasterView && choice.id == this->selectedChoiceId_;
        const bool showSelectedOutline =
            isSelectedChoice && !this->suppressSelectedChoiceOutline_;
        if (showSelectedOutline ||
            (!isSelectedChoice && !hasVotedForThis && isWinner))
        {
            const QColor outlineColor =
                showSelectedOutline ? rowTextColor : accentColor;
            rowButton->setOutlineColor(outlineColor);
            rowButton->setStyleSheet(
                QString("QPushButton#PollChoiceButton { border-color: %1; %2 }")
                    .arg(outlineColor.name(),
                         hasVotedForThis ? "border-width: 2px;" : ""));
        }

        if (!qFuzzyCompare(startFillProgress + 1.0, targetFillProgress + 1.0))
        {
            auto *fillAnimation = new QVariantAnimation(rowButton);
            fillAnimation->setDuration(420);
            fillAnimation->setEasingCurve(pollChoiceFillCurve());
            fillAnimation->setStartValue(startFillProgress);
            fillAnimation->setEndValue(targetFillProgress);
            QObject::connect(
                fillAnimation, &QVariantAnimation::valueChanged, rowButton,
                [this, rowButton, choiceId = choice.id](const QVariant &value) {
                    const double progress = value.toDouble();
                    rowButton->setFillProgress(progress);
                    this->pollChoiceFillProgress_.insert(choiceId, progress);
                });
            QObject::connect(
                fillAnimation, &QVariantAnimation::finished, rowButton,
                [this, rowButton, choiceId = choice.id, targetFillProgress] {
                    rowButton->setFillProgress(targetFillProgress);
                    this->pollChoiceFillProgress_.insert(choiceId,
                                                         targetFillProgress);
                });
            QObject::connect(fillAnimation, &QVariantAnimation::finished,
                             fillAnimation, &QObject::deleteLater);
            fillAnimation->start();
        }
        else
        {
            this->pollChoiceFillProgress_.insert(choice.id, targetFillProgress);
        }

        auto *rowLayout = new QHBoxLayout(rowButton);
        rowLayout->setContentsMargins(sectionPad, 0, sectionPad, 0);
        rowLayout->setSpacing(0);
        rowLayout->setAlignment(Qt::AlignVCenter);

        auto *accent = new QWidget(rowButton);
        accent->setFixedWidth(accentWidth);
        accent->setFixedHeight(accentHeight);
        accent->setStyleSheet(QString("background:%1; border-radius:%2px;")
                                  .arg(accentColor.name(QColor::HexArgb))
                                  .arg(std::max(1, accentWidth / 2)));
        rowLayout->addWidget(accent, 0, Qt::AlignVCenter);
        rowLayout->addSpacing(accentTextSpacing);

        auto *nameLabel = new QLabel(choice.title, rowButton);
        nameLabel->setObjectName("PollOutcomeName");
        nameLabel->setFont(buttonFont);
        nameLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        nameLabel->setContentsMargins(0, 0, 0, textOpticalLift);
        nameLabel->setFixedHeight(outcomeRowHeight);
        nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        nameLabel->setStyleSheet(
            QString("color: %1; background: transparent; border: none;")
                .arg(rowTextColor.name(QColor::HexArgb)));
        rowLayout->addWidget(nameLabel, 1, Qt::AlignVCenter);
        rowLayout->addSpacing(std::max(2, rowSpacing - 1));

        auto *metaWidget = new QWidget(rowButton);
        metaWidget->setFixedHeight(outcomeRowHeight);
        auto *metaLayout = new QHBoxLayout(metaWidget);
        metaLayout->setContentsMargins(0, 0, 0, 0);
        metaLayout->setSpacing(std::max(3, int(4 * effectiveScale)));
        metaLayout->setAlignment(Qt::AlignVCenter);

        auto *votesValue =
            new QLabel(locale.toString(choice.totalVotes), metaWidget);
        votesValue->setObjectName("PollVotes_" + choice.id);
        votesValue->setFont(compactMetaFont);
        votesValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        votesValue->setFixedHeight(outcomeRowHeight);
        votesValue->setFixedWidth(votesValueWidth);
        votesValue->setStyleSheet(
            QString("color: %1; background: transparent; border: none;")
                .arg(rowTextColor.name(QColor::HexArgb)));
        metaLayout->addWidget(votesValue);

        auto *votesLabel = new QLabel("VOTES", metaWidget);
        votesLabel->setObjectName("PollOutcomeStats");
        votesLabel->setFont(compactMetaFont);
        votesLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        votesLabel->setFixedHeight(outcomeRowHeight);
        votesLabel->setStyleSheet(
            QString("color: %1; background: transparent; border: none;")
                .arg(rowMutedTextColor.name(QColor::HexArgb)));
        metaLayout->addWidget(votesLabel);

        auto *pctLabel = new QLabel(QString::number(pct) + "%", rowButton);
        pctLabel->setObjectName("PollPct_" + choice.id);
        pctLabel->setFont(compactPctFont);
        pctLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        pctLabel->setFixedHeight(outcomeRowHeight);
        pctLabel->setFixedWidth(pctValueWidth);
        if (!hasVotedForThis && isWinner)
        {
            pctLabel->setStyleSheet(
                QString("color:%1; background: transparent; border: none;")
                    .arg(accentColor.name()));
        }
        else
        {
            pctLabel->setStyleSheet(
                QString("color: %1; background: transparent; border: none;")
                    .arg(rowTextColor.name(QColor::HexArgb)));
        }
        metaLayout->addWidget(pctLabel);

        rowLayout->addWidget(metaWidget, 0, Qt::AlignVCenter);

        if (!broadcasterView)
        {
            QObject::connect(rowButton, &QPushButton::clicked, this,
                             [this, choiceId = choice.id] {
                                 if (choiceId != this->selectedChoiceId_)
                                 {
                                     this->suppressSelectedChoiceOutline_ =
                                         false;
                                 }
                                 this->selectedChoiceId_ = choiceId;
                                 this->updateUi();
                             });
        }
        outcomesLayout->addWidget(rowButton);
    }

    outcomesLayout->addStretch(1);

    this->outcomesScrollArea_->setWidget(outcomesWidget);
    QObject::connect(this->outcomesScrollArea_->verticalScrollBar(),
                     &QScrollBar::valueChanged, this, [this](int value) {
                         this->outcomesScrollValue_ = value;
                     });
    QTimer::singleShot(0, this, [this] {
        if (this->outcomesScrollArea_ != nullptr)
        {
            this->outcomesScrollArea_->verticalScrollBar()->setValue(
                this->outcomesScrollValue_);
        }
    });
    layout->addWidget(outcomesPanel, 1);
    layout->addSpacing(sectionSpacing);

    if (broadcasterView)
    {
        QTimer::singleShot(0, this, [this] {
            this->fitVoteOptionsList();
        });
        return;
    }

    const bool votingOpen =
        poll.status.compare("ACTIVE", Qt::CaseInsensitive) == 0 &&
        this->remainingPollSeconds() > 0;
    const bool canBuyExtraVotes = poll.channelPointsVotingEnabled &&
                                  this->currentUserTotalVotes() > 0 &&
                                  poll.pointsPerVote > 0 && votingOpen;
    const bool canCastFreeVote =
        this->currentUserTotalVotes() == 0 && votingOpen;
    const int compactControlHeight = std::max(
        10,
        std::max(int(20 * effectiveScale),
                 uiFont.pointSize() + std::max(1, int(2 * effectiveScale))));

    this->bottomWidget_ = new QWidget(this);
    this->bottomWidget_->setObjectName("PollBottomBar");
    auto *bottomWrapperLayout = new QVBoxLayout(this->bottomWidget_);
    const int wrapperPad = std::max(3, int(5 * rawScale));
    bottomWrapperLayout->setContentsMargins(wrapperPad, 0, wrapperPad,
                                            wrapperPad);
    bottomWrapperLayout->setSpacing(0);

    auto *bottomCard = new QWidget(this->bottomWidget_);
    bottomCard->setObjectName("PollCard");
    auto *bottomLayout = new QVBoxLayout(bottomCard);
    bottomLayout->setContentsMargins(sectionPad, sectionPad, sectionPad,
                                     sectionPad);
    bottomLayout->setSpacing(sectionSpacing);

    {
        auto *infoRow = new QHBoxLayout();
        infoRow->setSpacing(rowSpacing);

        QString voteHeader = "Vote Submitted";
        if (canCastFreeVote)
        {
            voteHeader = "Cast Vote";
        }
        else if (canBuyExtraVotes)
        {
            voteHeader = "Cast Additional Vote";
        }
        else if (!votingOpen)
        {
            voteHeader = "Voting Ended";
        }

        auto *leftInfo = new QLabel(voteHeader, bottomCard);
        leftInfo->setObjectName("PollSectionTitle");
        leftInfo->setFont(buttonFont);
        infoRow->addWidget(leftInfo);
        infoRow->addStretch(1);

        if (poll.channelPointsVotingEnabled)
        {
            auto *balance = new QLabel(
                QString("Bal: %1").arg(
                    formatChannelPoints(this->channel_->channelPointBalance())),
                bottomCard);
            balance->setObjectName("PollBalanceLabel");
            balance->setFont(uiFont);
            infoRow->addWidget(balance);
        }
        bottomLayout->addLayout(infoRow);
    }

    auto *voteButton = new QPushButton(bottomCard);
    voteButton->setObjectName("PollPrimaryButton");
    voteButton->setFont(buttonFont);
    voteButton->setFixedHeight(compactControlHeight);
    if (canCastFreeVote)
    {
        voteButton->setText(this->actionInFlight_ ? "Voting..." : "Vote");
    }
    else if (canBuyExtraVotes)
    {
        voteButton->setText(
            this->actionInFlight_
                ? "Voting..."
                : QString("Vote (%1)").arg(formatCompact(poll.pointsPerVote)));
    }
    else if (!votingOpen)
    {
        voteButton->setText("Voting ended");
    }
    else if (poll.channelPointsVotingEnabled &&
             this->currentUserTotalVotes() > 0)
    {
        voteButton->setText("Point cost unavailable");
    }
    else
    {
        voteButton->setText("Already voted");
    }

    voteButton->setEnabled(!this->actionInFlight_ &&
                           !this->selectedChoiceId_.isEmpty() &&
                           (canCastFreeVote || canBuyExtraVotes));
    QObject::connect(voteButton, &QPushButton::clicked, this,
                     [this, canBuyExtraVotes] {
                         this->castVote(canBuyExtraVotes);
                     });
    bottomLayout->addWidget(voteButton);
    bottomWrapperLayout->addWidget(bottomCard);
    this->mainLayout_->addWidget(this->bottomWidget_);

    QTimer::singleShot(0, this, [this] {
        this->fitVoteOptionsList();
    });
}

void PollDialog::createPoll()
{
    const auto title = this->draftTitle_.trimmed();
    if (title.isEmpty())
    {
        this->channel_->addSystemMessage("Poll title cannot be empty.");
        return;
    }

    QStringList choices;
    QSet<QString> uniqueChoices;
    if (this->draftChoices_.value(0).trimmed().isEmpty() ||
        this->draftChoices_.value(1).trimmed().isEmpty())
    {
        this->channel_->addSystemMessage(
            "The first two poll responses are required.");
        return;
    }

    for (const auto &draftChoice : this->draftChoices_)
    {
        const auto trimmed = draftChoice.trimmed();
        if (trimmed.isEmpty())
        {
            continue;
        }
        const auto normalized = trimmed.toCaseFolded();
        if (uniqueChoices.contains(normalized))
        {
            continue;
        }
        uniqueChoices.insert(normalized);
        choices.push_back(trimmed);
    }

    if (choices.size() < MIN_CHOICES)
    {
        this->channel_->addSystemMessage("A poll needs at least 2 responses.");
        return;
    }

    if (choices.size() > MAX_CHOICES)
    {
        this->channel_->addSystemMessage(
            "A poll can have at most 5 responses.");
        return;
    }

    if (this->draftEnableAdditionalVotes_ && this->draftPointsPerVote_ <= 0)
    {
        this->channel_->addSystemMessage(
            "Additional vote cost must be greater than 0.");
        return;
    }

    if (this->channel_->roomId().isEmpty())
    {
        this->channel_->addSystemMessage(
            "Poll creation is not ready yet. Try again after the channel "
            "finishes joining.");
        return;
    }

    QString authError;
    const auto auth = MoltorinoAuth::resolveModerationToken(
        this->channel_->roomId(), this->channel_->getName(), &authError);
    if (!auth.hasToken())
    {
        this->channel_->addSystemMessage(
            authError.isEmpty() ? moltorinoAuthRequiredMessage("creating polls")
                                : authError);
        return;
    }

    this->actionInFlight_ = true;
    this->updateUi();

    QPointer<PollDialog> self = this;
    TwitchGql::createPollEvent(
        this->channel_->roomId(), title, choices, this->draftDurationSeconds_,
        this->draftEnableAdditionalVotes_
            ? std::optional<int>(this->draftPointsPerVote_)
            : std::nullopt,
        auth.token,
        [self] {
            if (!self)
            {
                return;
            }
            self->channel_->refreshPollIfStale(true);
            self->close();
        },
        [self](const QString &error) {
            if (!self)
            {
                return;
            }
            self->actionInFlight_ = false;
            self->channel_->addSystemMessage(normalizePollCreateError(error));
            self->updateUi();
        });
}

void PollDialog::castVote(bool extraVote)
{
    if (this->isBroadcasterView() || !this->currentPoll_ ||
        this->selectedChoiceId_.isEmpty())
    {
        return;
    }
    if (this->currentPoll_->status.compare("ACTIVE", Qt::CaseInsensitive) !=
            0 ||
        this->remainingPollSeconds() <= 0)
    {
        this->channel_->addSystemMessage("Voting has ended.");
        return;
    }

    const auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (account->isAnon())
    {
        this->channel_->addSystemMessage("You must be logged in to vote.");
        return;
    }
    if (account->getUserId().isEmpty())
    {
        this->channel_->addSystemMessage("Your Twitch user ID is not available "
                                         "yet. Try again after reconnecting.");
        return;
    }

    QString authError;
    const auto auth = MoltorinoAuth::resolveCurrentUserToken(&authError);
    if (!auth.hasToken())
    {
        this->channel_->addSystemMessage(
            authError.isEmpty()
                ? moltorinoAuthRequiredMessage("voting in polls")
                : authError);
        return;
    }

    this->suppressSelectedChoiceOutline_ = true;
    this->actionInFlight_ = true;
    this->updateUi();

    const auto pollId = this->currentPoll_->id;
    const auto choiceId = this->selectedChoiceId_;
    const auto userId =
        auth.userId.isEmpty() ? account->getUserId() : auth.userId;
    const auto pointsPerVote =
        this->currentPoll_->channelPointsVotingEnabled
            ? std::optional<int>(this->currentPoll_->pointsPerVote)
            : std::nullopt;
    QPointer<PollDialog> self = this;
    TwitchGql::voteInPoll(
        pollId, choiceId, userId, extraVote ? 1 : 0, pointsPerVote, auth.token,
        [self, choiceId, extraVote] {
            if (!self)
            {
                return;
            }

            if (self->currentPoll_)
            {
                auto matched = false;
                for (auto &vote : self->currentPoll_->selfVotes)
                {
                    if (vote.choiceId == choiceId)
                    {
                        if (extraVote)
                        {
                            vote.channelPointsVotes += 1;
                        }
                        else
                        {
                            vote.freeVotes = 1;
                        }
                        matched = true;
                        break;
                    }
                }
                if (!matched)
                {
                    TwitchChannel::PollSelfVote vote;
                    vote.choiceId = choiceId;
                    if (extraVote)
                    {
                        vote.channelPointsVotes = 1;
                    }
                    else
                    {
                        vote.freeVotes = 1;
                    }
                    self->currentPoll_->selfVotes.push_back(std::move(vote));
                }
                self->channel_->setActivePoll(*self->currentPoll_);
                self->channel_->refreshPollIfStale(true);
                if (self->currentPoll_->channelPointsVotingEnabled)
                {
                    self->channel_->refreshChannelPointsIfStale(true);
                }
            }

            self->actionInFlight_ = false;
            if (getSettings()->pollAutoCloseDialog)
            {
                self->close();
                return;
            }
            self->updateUi();
        },
        [self](const QString &error) {
            if (!self)
            {
                return;
            }
            self->actionInFlight_ = false;
            self->channel_->addSystemMessage(normalizePollVoteError(error));
            self->updateUi();
        });
}

void PollDialog::fitCreateOptionsList()
{
    if (this->currentPoll_ || this->scrollArea_ == nullptr ||
        this->outcomesScrollArea_ == nullptr ||
        this->createOutcomesPanel_ == nullptr)
    {
        return;
    }

    auto *layout = qobject_cast<QVBoxLayout *>(this->activeWidget_->layout());
    if (layout == nullptr)
    {
        return;
    }

    const int contentHeight = this->scrollArea_->viewport()->height();
    if (contentHeight <= 0)
    {
        return;
    }

    const QMargins margins = layout->contentsMargins();
    int reservedHeight = margins.top() + margins.bottom();
    int visibleItems = 0;
    for (int i = 0; i < layout->count(); ++i)
    {
        auto *item = layout->itemAt(i);
        if (item == nullptr)
        {
            continue;
        }

        if (item->widget() == this->createOutcomesPanel_)
        {
            ++visibleItems;
            continue;
        }

        const auto hint = item->sizeHint();
        if (hint.isValid())
        {
            reservedHeight += hint.height();
            ++visibleItems;
        }
    }
    if (visibleItems > 1)
    {
        reservedHeight += layout->spacing() * (visibleItems - 1);
    }

    const int available = contentHeight - reservedHeight;
    if (available <= 0)
    {
        return;
    }

    const int wanted =
        this->createOutcomesPanel_->property("pollWantedHeight").toInt();
    if (wanted <= 0)
    {
        return;
    }

    const int fittedHeight = std::min(std::max(24, wanted), available);
    this->createOutcomesPanel_->setMinimumHeight(fittedHeight);
    this->createOutcomesPanel_->setMaximumHeight(QWIDGETSIZE_MAX);
    this->outcomesScrollArea_->setMinimumHeight(0);
    this->outcomesScrollArea_->setMaximumHeight(QWIDGETSIZE_MAX);
}

void PollDialog::fitVoteOptionsList()
{
    if (!this->currentPoll_ || this->scrollArea_ == nullptr ||
        this->outcomesScrollArea_ == nullptr ||
        this->voteQuestionCard_ == nullptr ||
        this->voteOutcomesPanel_ == nullptr)
    {
        return;
    }

    auto *layout = qobject_cast<QVBoxLayout *>(this->activeWidget_->layout());
    if (layout == nullptr)
    {
        return;
    }

    const int contentHeight = this->scrollArea_->viewport()->height();
    if (contentHeight <= 0)
    {
        return;
    }

    const QMargins margins = layout->contentsMargins();
    int reservedHeight = margins.top() + margins.bottom();
    int visibleItems = 0;
    for (int i = 0; i < layout->count(); ++i)
    {
        auto *item = layout->itemAt(i);
        if (item == nullptr)
        {
            continue;
        }

        if (item->widget() == this->voteOutcomesPanel_)
        {
            ++visibleItems;
            continue;
        }

        const auto hint = item->sizeHint();
        if (hint.isValid())
        {
            reservedHeight += hint.height();
            ++visibleItems;
        }
    }
    if (visibleItems > 1)
    {
        reservedHeight += layout->spacing() * (visibleItems - 1);
    }

    const int wanted =
        this->voteOutcomesPanel_->property("pollWantedHeight").toInt();
    if (wanted <= 0)
    {
        return;
    }

    const int available = contentHeight - reservedHeight;
    const int fittedHeight = std::max(1, available);
    this->voteOutcomesPanel_->setMinimumHeight(fittedHeight);
    this->voteOutcomesPanel_->setMaximumHeight(fittedHeight);
    this->outcomesScrollArea_->setMinimumHeight(0);
    this->outcomesScrollArea_->setMaximumHeight(fittedHeight);
    this->activeWidget_->setMinimumHeight(0);
    this->activeWidget_->updateGeometry();
}

void PollDialog::refreshStyle()
{
    const float rawScale = this->scale();
    const float effectiveScale = contentScale(rawScale);
    const int radius = std::max(1, int(2 * rawScale));
    const int inputPaddingY = 0;
    const int inputPaddingX = std::max(4, int(5 * effectiveScale));
    const int inputMinHeight = std::max(14, int(20 * effectiveScale));
    const int compactControlPaddingY = 0;
    const int compactControlPaddingX = std::max(4, int(5 * effectiveScale));
    const int compactControlMinHeight = std::max(14, int(20 * effectiveScale));
    const int scrollbarWidth = std::max(3, int(4 * effectiveScale));
    const int scrollbarRadius = std::max(1, int(2 * effectiveScale));
    const int scrollbarMinHeight = std::max(12, int(16 * effectiveScale));
    auto windowBackground = this->theme->window.background;
    auto textColor = this->theme->window.text;
    auto mutedColor = textColor;
    mutedColor.setAlpha(160);
    auto borderColor = this->theme->splits.header.border;
    auto cardBackground = this->theme->splits.header.background;
    auto inputBackground = this->theme->splits.input.background;
    auto accentColor = this->theme->tabs.selected.backgrounds.regular;
    auto accentTextColor = this->theme->tabs.selected.text;

    this->closeButton_->setColor(textColor);
    this->headerTitleLabel_->setFont(
        getApp()->getFonts()->getFont(FontStyle::UiMediumBold, effectiveScale));
    this->headerSubtitleLabel_->setFont(
        getApp()->getFonts()->getFont(FontStyle::UiMedium, effectiveScale));
    this->headerSubtitleLabel_->setStyleSheet(QString());

    this->headerWidget_->layout()->setContentsMargins(
        std::max(2, int(4 * rawScale)), std::max(2, int(3 * rawScale)),
        std::max(2, int(4 * rawScale)), std::max(2, int(3 * rawScale)));
    this->headerWidget_->layout()->setSpacing(std::max(2, int(3 * rawScale)));

    this->mainLayout_->setContentsMargins(
        std::max(3, int(5 * rawScale)), std::max(3, int(5 * rawScale)),
        std::max(3, int(5 * rawScale)), std::max(3, int(5 * rawScale)));
    this->mainLayout_->setSpacing(0);
    if (auto *separator = this->findChild<QWidget *>("PollDialogSeparator"))
    {
        separator->setFixedHeight(scaledSeparatorHeight(rawScale));
    }

    this->setStyleSheet(QString(R"(
            QWidget#PollDialogRoot {
                background: %1;
            }
            QFrame#PollDialogSeparator {
                background: %3;
            }
            QWidget#PollCard,
            QWidget#PollQuestionCard {
                background: %2;
                border: 1px solid %3;
                border-radius: %4px;
            }
            QScrollArea#PollScrollArea,
            QScrollArea#PollOutcomesScrollArea,
            QWidget#PollDialogContent,
            QWidget#PollOutcomesContent,
            QWidget#PollBottomBar {
                border: none;
                background: transparent;
            }
            QWidget#PollOutcomesPanel {
                background: %2;
                border: 1px solid %3;
                border-radius: %4px;
            }
            QLabel#PollHeaderTitle {
                color: %5;
                font-weight: 700;
            }
            QLabel#PollCurrentTitle {
                color: %5;
                font-weight: 700;
            }
            QLabel#PollSectionTitle {
                color: %6;
                font-weight: 600;
            }
            QLabel#PollHeaderSubtitle,
            QLabel#PollCountLabel,
            QLabel#PollBalanceLabel,
            QLabel#PollInfoLabel,
            QLabel#PollOutcomeStats,
            QLabel#PollCountdown {
                color: %6;
            }
            QLabel#PollOutcomeName {
                color: %5;
                font-weight: 600;
            }
            QLineEdit#PollTitleInput,
            QLineEdit#PollOptionInput,
            QLineEdit#PollPointsInput {
                background: %7;
                color: %5;
                border: 1px solid %3;
                border-radius: %4px;
                padding: %8px %9px;
                min-height: %10px;
            }
            QComboBox#PollCreateDurationCombo {
                background: %7;
                color: %5;
                border: 1px solid %3;
                border-radius: %4px;
                padding: %11px %12px;
                min-height: %13px;
            }
            QComboBox#PollCreateDurationCombo QAbstractItemView {
                background: %1;
                color: %5;
                border: 1px solid %3;
                outline: none;
                selection-background-color: %7;
                selection-color: %5;
            }
            QPushButton#PollCreateStartButton,
            QPushButton#PollPrimaryButton {
                border: 1px solid transparent;
                border-radius: %4px;
                font-weight: 600;
                padding: %11px %12px;
                min-height: %13px;
                background: %14;
                color: %15;
            }
            QPushButton#PollChoiceButton {
                background: %7;
                color: %5;
                border: 1px solid %3;
                border-radius: %4px;
                text-align: left;
            }
            QPushButton#PollChoiceButton:hover {
                background: %16;
            }
            QPushButton:disabled {
                color: %6;
            }
            QCheckBox#PollCheckBox {
                color: %5;
            }
            QScrollArea#PollOutcomesScrollArea QScrollBar:vertical {
                background: transparent;
                width: %17px;
                margin: 0;
            }
            QScrollArea#PollOutcomesScrollArea QScrollBar::handle:vertical {
                background: %6;
                border-radius: %18px;
                min-height: %19px;
            }
            QScrollArea#PollOutcomesScrollArea QScrollBar::add-line:vertical,
            QScrollArea#PollOutcomesScrollArea QScrollBar::sub-line:vertical,
            QScrollArea#PollOutcomesScrollArea QScrollBar::add-page:vertical,
            QScrollArea#PollOutcomesScrollArea QScrollBar::sub-page:vertical {
                background: transparent;
                height: 0;
            }
        )")
                            .arg(windowBackground.name())
                            .arg(cardBackground.name())
                            .arg(borderColor.name())
                            .arg(radius)
                            .arg(textColor.name())
                            .arg(mutedColor.name(QColor::HexArgb))
                            .arg(inputBackground.name())
                            .arg(inputPaddingY)
                            .arg(inputPaddingX)
                            .arg(inputMinHeight)
                            .arg(compactControlPaddingY)
                            .arg(compactControlPaddingX)
                            .arg(compactControlMinHeight)
                            .arg(accentColor.name())
                            .arg(accentTextColor.name())
                            .arg(this->theme->isLightTheme()
                                     ? cardBackground.darker(104).name()
                                     : cardBackground.lighter(108).name())
                            .arg(scrollbarWidth)
                            .arg(scrollbarRadius)
                            .arg(scrollbarMinHeight));
}

void PollDialog::applySizeConstraints(bool preserveCurrentPosition)
{
    auto *screen = this->screen();
    if (screen == nullptr)
    {
        screen = QGuiApplication::screenAt(QCursor::pos());
    }
    if (screen == nullptr)
    {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr)
    {
        return;
    }

    const auto available = screen->availableGeometry();
    const int screenMargin = std::max(12, int(16 * this->scale()));
    const int maxWidth = std::max(1, available.width() - screenMargin * 2);
    const int maxHeight = std::max(1, available.height() - screenMargin * 2);

    const bool createMode = !this->currentPoll_.has_value();
    const bool useContentDrivenHeight =
        this->currentPoll_.has_value() || createMode;
    const int targetWidth =
        std::min(int(this->scaleIndependentWidth() * this->scale()), maxWidth);
    int targetHeight = std::min(
        int(this->scaleIndependentHeight() * this->scale()), maxHeight);

    if (useContentDrivenHeight)
    {
        auto *container = this->getLayoutContainer();
        if (container != nullptr)
        {
            container->ensurePolished();
            if (auto *layout = container->layout())
            {
                layout->invalidate();
                layout->activate();
            }
        }

        if (this->headerWidget_ != nullptr)
        {
            this->headerWidget_->ensurePolished();
        }

        if (this->activeWidget_ != nullptr)
        {
            this->activeWidget_->ensurePolished();
            if (auto *layout = this->activeWidget_->layout())
            {
                layout->invalidate();
                layout->activate();
            }
        }

        int contentHeight = 0;
        QMargins mainMargins;
        int contentWidth = targetWidth;
        if (this->mainLayout_ != nullptr)
        {
            mainMargins = this->mainLayout_->contentsMargins();
            contentHeight += mainMargins.top() + mainMargins.bottom();
            contentWidth -= mainMargins.left() + mainMargins.right();
        }
        contentWidth = std::max(1, contentWidth - 2);

        if (this->headerWidget_ != nullptr)
        {
            int headerHeight = this->headerWidget_->sizeHint().height();
            if (auto *layout = this->headerWidget_->layout();
                layout != nullptr && layout->hasHeightForWidth())
            {
                headerHeight = std::max(
                    headerHeight, layout->totalHeightForWidth(contentWidth));
            }
            contentHeight += headerHeight;
        }
        contentHeight += scaledSeparatorHeight(this->scale());
        if (this->activeWidget_ != nullptr)
        {
            int activeHeight = 0;
            if (auto *layout = this->activeWidget_->layout();
                layout != nullptr && layout->hasHeightForWidth())
            {
                activeHeight = layout->totalHeightForWidth(contentWidth);
            }
            if (activeHeight == 0)
            {
                activeHeight = this->activeWidget_->sizeHint().height();
            }
            contentHeight += activeHeight;
        }

        if (this->bottomWidget_ != nullptr)
        {
            int bottomHeight = 0;
            if (auto *layout = this->bottomWidget_->layout();
                layout != nullptr && layout->hasHeightForWidth())
            {
                bottomHeight = layout->totalHeightForWidth(contentWidth);
            }
            if (bottomHeight == 0)
            {
                bottomHeight = this->bottomWidget_->sizeHint().height();
            }
            contentHeight += bottomHeight;
        }

        contentHeight += std::max(4, int(6 * this->scale()));

        if (contentHeight > 0)
        {
            targetHeight = std::min(contentHeight, maxHeight);
        }
    }

    if (this->width() != targetWidth || this->height() != targetHeight)
    {
        if (preserveCurrentPosition)
        {
            this->resize(targetWidth, targetHeight);
        }
        else
        {
            this->setFixedSize(targetWidth, targetHeight);
        }
    }
}

}  // namespace chatterino

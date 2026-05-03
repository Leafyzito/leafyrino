// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/SplitPollPanel.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitCommon.hpp"

#include <QEnterEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace {

using namespace chatterino;

constexpr int POLL_INTERVAL_MS = 5'000;

QString formatCountdownMs(qint64 ms)
{
    if (ms <= 0)
    {
        return QStringLiteral("Ending…");
    }
    const qint64 totalSec = (ms + 999) / 1000;
    const qint64 m = totalSec / 60;
    const qint64 s = totalSec % 60;
    if (m > 0)
    {
        return QStringLiteral("%1m %2s left")
            .arg(m)
            .arg(s, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1s left").arg(s);
}

QString winningChoiceIdFromVotes(const TwitchGql::ViewablePoll &poll)
{
    QString id;
    int best = -1;
    for (const auto &ch : poll.choices)
    {
        const int v = std::max(0, ch.votesTotal);
        if (v > best)
        {
            best = v;
            id = ch.id;
        }
    }
    return id;
}

TwitchGql::ViewablePoll mergePollWithPriorViewerVotes(
    TwitchGql::ViewablePoll poll,
    const std::optional<TwitchGql::ViewablePoll> &prior)
{
    if (!prior.has_value() || prior->id != poll.id)
    {
        return poll;
    }
    if (!poll.viewerVotedChoiceIds.empty())
    {
        return poll;
    }
    if (!prior->viewerVotedChoiceIds.empty())
    {
        poll.viewerVotedChoiceIds = prior->viewerVotedChoiceIds;
    }
    return poll;
}

}  // namespace

namespace chatterino {

class PollChoiceRow final : public QWidget
{
public:
    explicit PollChoiceRow(QWidget *parent)
        : QWidget(parent)
    {
        this->setMouseTracking(true);
        this->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    }

    void setPickHandler(std::function<void(const QString &)> handler)
    {
        this->onPick_ = std::move(handler);
    }

    [[nodiscard]] const QString &pollChoiceId() const
    {
        return this->choiceId_;
    }

    void configure(const Theme *theme, float scale, const QString &choiceId,
                   const QString &title, double fill01, int votes, int pctInt,
                   bool terminal, bool isWinner, bool clickable,
                   bool dimAfterVote, bool viewerPickedWhileActive)
    {
        this->theme_ = theme;
        this->scale_ = scale;
        this->choiceId_ = choiceId;
        this->title_ = title;
        this->fill01_ = std::clamp(fill01, 0.0, 1.0);
        this->votes_ = votes;
        this->pctInt_ = pctInt;
        this->terminal_ = terminal;
        this->winner_ = isWinner;
        this->clickable_ = clickable;
        this->dimAfterVote_ = dimAfterVote;
        this->viewerPicked_ = viewerPickedWhileActive;
        this->hover_ = false;

        if (theme != nullptr)
        {
            this->trackBg_ = theme->splits.background.darker(180);
            this->border_ = theme->splits.header.border;
            this->text_ = theme->splits.header.text;
            this->accentColor_ = theme->accent;
            this->fill_ = theme->isLightTheme() ? QColor(145, 70, 255, 200)
                                                : QColor(169, 112, 255, 200);
            const QColor &br = this->border_;
            const QColor &bg = theme->splits.background;
            this->hoverStroke_ = QColor::fromRgbF(
                (br.redF() + bg.redF()) * 0.5,
                (br.greenF() + bg.greenF()) * 0.5,
                (br.blueF() + bg.blueF()) * 0.5, std::max(br.alphaF(), 0.55f));
            this->hoverFill_ = QColor::fromRgbF(
                this->hoverStroke_.redF(), this->hoverStroke_.greenF(),
                this->hoverStroke_.blueF(), 0.12f);
        }
        this->setCursor(clickable ? Qt::PointingHandCursor : Qt::ArrowCursor);
        const int h = static_cast<int>(24 * std::max(1.0f, scale));
        this->setMinimumHeight(h);
        this->update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const QRect r = this->rect().adjusted(0, 0, -1, -1);
        const float s = std::max(1.0f, this->scale_);
        const int radius = static_cast<int>(4 * s);

        if (this->theme_ == nullptr)
        {
            return;
        }

        p.setPen(Qt::NoPen);
        p.setBrush(this->trackBg_);
        p.drawRoundedRect(r, radius, radius);

        const int fw = static_cast<int>(std::lround(
            std::clamp(this->fill01_ * r.width(), 0.0, double(r.width()))));
        if (fw > 1)
        {
            QPainterPath clip;
            clip.addRoundedRect(r, radius, radius);
            p.setClipPath(clip);
            QRect fr(r.left(), r.top(), fw, r.height());
            QColor fill = this->fill_;
            if (this->terminal_)
            {
                fill.setAlpha(140);
            }
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawRect(fr);
            p.setClipping(false);
        }

        if (this->clickable_ && this->hover_)
        {
            QPainterPath clip;
            clip.addRoundedRect(r, radius, radius);
            p.setClipPath(clip);
            p.setPen(Qt::NoPen);
            p.setBrush(this->hoverFill_);
            p.drawRect(r.adjusted(1, 1, -1, -1));
            p.setClipping(false);
            p.setPen(QPen(this->hoverStroke_, 1));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r.adjusted(1, 1, -1, -1), std::max(2, radius - 1),
                              std::max(2, radius - 1));
        }
        else if (this->dimAfterVote_ && !this->terminal_)
        {
            p.fillRect(r, QColor(0, 0, 0, 55));
        }

        QColor txt = this->text_;
        if (this->dimAfterVote_ && !this->terminal_)
        {
            txt = QColor::fromRgbF(txt.redF() * 0.65, txt.greenF() * 0.65,
                                   txt.blueF() * 0.65, txt.alphaF());
        }
        p.setPen(txt);
        QFont f = this->font();
        const bool winnerUi = this->terminal_ && this->winner_;
        f.setBold(!this->terminal_ || winnerUi);
        p.setFont(f);
        const int pad = static_cast<int>(6 * s);
        const QString right =
            QStringLiteral("%1% (%2)").arg(this->pctInt_).arg(this->votes_);
        const QFontMetrics fm(f);
        const int rightW = fm.horizontalAdvance(right) + pad;

        if (winnerUi)
        {
            const int iconCol = splitHeaderIconColumnWidth(s);
            const QString iconPath =
                this->theme_->isLightTheme()
                    ? QStringLiteral(":/buttons/pollTrophy-lightMode.svg")
                    : QStringLiteral(":/buttons/pollTrophy-darkMode.svg");
            const int vMargin = std::max(1, static_cast<int>(std::ceil(s)));
            const int innerH = std::max(1, r.height() - 2 * vMargin);
            const int slot = std::min(iconCol, innerH);
            const QPixmap trophyPm = QIcon(iconPath).pixmap(slot, slot);
            const int gap = static_cast<int>(std::round(3 * s));
            const int leading = trophyPm.isNull() ? 0 : (iconCol + gap);
            const int textLeft = pad + leading;
            const int leftMax =
                std::max(40, r.width() - textLeft - rightW - pad);
            const QString elided =
                fm.elidedText(this->title_, Qt::ElideRight, leftMax);
            if (!trophyPm.isNull())
            {
                const int colLeft = r.left() + pad;
                const int ix = colLeft + (iconCol - slot) / 2;
                const int iy = r.top() + vMargin + (innerH - slot) / 2;
                p.drawPixmap(QRect(ix, iy, slot, slot), trophyPm,
                             trophyPm.rect());
            }
            p.drawText(r.adjusted(textLeft, 0, -rightW - pad, 0),
                       Qt::AlignVCenter | Qt::AlignLeft, elided);
        }
        else
        {
            const int leftMax = std::max(40, r.width() - rightW - pad);
            const QString elided =
                fm.elidedText(this->title_, Qt::ElideRight, leftMax);
            p.drawText(r.adjusted(pad, 0, -rightW - pad, 0),
                       Qt::AlignVCenter | Qt::AlignLeft, elided);
        }
        p.drawText(r.adjusted(0, 0, -pad, 0), Qt::AlignVCenter | Qt::AlignRight,
                   right);

        QColor frameColor = this->border_;
        int frameW = 1;
        if (winnerUi)
        {
            frameColor = QColor(0, 188, 125);
            frameW = 2;
        }
        else if (this->viewerPicked_ && !this->terminal_)
        {
            frameColor = this->accentColor_;
            frameW = 2;
        }
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(frameColor, frameW));
        const int inset = frameW / 2;
        p.drawRoundedRect(r.adjusted(inset, inset, -inset, -inset),
                          std::max(2, radius - inset),
                          std::max(2, radius - inset));
    }

    void enterEvent(QEnterEvent *) override
    {
        if (!this->clickable_)
        {
            return;
        }
        this->hover_ = true;
        this->update();
    }

    void leaveEvent(QEvent *) override
    {
        if (!this->hover_)
        {
            return;
        }
        this->hover_ = false;
        this->update();
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && this->clickable_ && this->onPick_)
        {
            this->onPick_(this->choiceId_);
        }
        QWidget::mousePressEvent(e);
    }

private:
    const Theme *theme_{};
    float scale_{1.f};
    QString choiceId_;
    QString title_;
    double fill01_{};
    int votes_{};
    int pctInt_{};
    bool terminal_{};
    bool winner_{};
    bool clickable_{};
    bool dimAfterVote_{};
    bool viewerPicked_{};
    bool hover_{};
    QColor trackBg_;
    QColor border_;
    QColor text_;
    QColor accentColor_;
    QColor fill_;
    QColor hoverStroke_;
    QColor hoverFill_;
    std::function<void(const QString &)> onPick_;
};

bool pollRowsMatchPoll(const std::vector<PollChoiceRow *> &rows,
                       const TwitchGql::ViewablePoll &poll)
{
    if (rows.size() != poll.choices.size())
    {
        return false;
    }
    for (size_t i = 0; i < rows.size(); ++i)
    {
        PollChoiceRow *row = rows[i];
        if (row == nullptr || row->pollChoiceId() != poll.choices[i].id)
        {
            return false;
        }
    }
    return true;
}

SplitPollPanel::SplitPollPanel(Split *split)
    : BaseWidget(split)
    , split_(split)
{
    this->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Minimum);
    this->hide();

    this->pollTimer_.setInterval(POLL_INTERVAL_MS);
    this->pollTimer_.setTimerType(Qt::CoarseTimer);
    QObject::connect(&this->pollTimer_, &QTimer::timeout, this, [this] {
        this->fetchPoll();
    });

    this->countdownTimer_.setInterval(1'000);
    this->countdownTimer_.setTimerType(Qt::CoarseTimer);
    QObject::connect(&this->countdownTimer_, &QTimer::timeout, this, [this] {
        this->tickCountdownDisplay();
    });

    this->lingerTimer_.setInterval(SplitPollPanel::LINGER_MS);
    this->lingerTimer_.setSingleShot(true);
    this->lingerTimer_.setTimerType(Qt::CoarseTimer);
    QObject::connect(&this->lingerTimer_, &QTimer::timeout, this, [this] {
        this->onLingerTimeout();
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

    this->collapsedTitle_ = new QLabel(topRow);
    this->collapsedTitle_->setSizePolicy(QSizePolicy::MinimumExpanding,
                                         QSizePolicy::Preferred);
    this->collapsedTitle_->setWordWrap(false);

    this->dismissButton_ = new QPushButton(topRow);
    this->dismissButton_->setFlat(true);
    this->dismissButton_->setCursor(Qt::PointingHandCursor);
    this->dismissButton_->setFocusPolicy(Qt::NoFocus);
    this->dismissButton_->setText(QStringLiteral("×"));
    this->dismissButton_->setToolTip(QStringLiteral("Dismiss for this poll"));
    QObject::connect(this->dismissButton_, &QPushButton::clicked, this, [this] {
        if (!this->current_.has_value())
        {
            return;
        }
        this->dismissedForPollId_ = this->current_->id;
        this->lastLivePoll_.reset();
        this->lingerTimer_.stop();
        this->lingering_ = false;
        this->lingerPollId_.clear();
        this->currentDisplayPollId_.clear();
        this->hidePollUi();
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
    hl->addWidget(this->collapsedTitle_, 1);
    hl->addWidget(this->expandButton_, 0, Qt::AlignRight | Qt::AlignVCenter);
    hl->addWidget(this->dismissButton_, 0, Qt::AlignRight | Qt::AlignVCenter);

    this->expandedWidget_ = new QWidget(this);
    this->expandedLayout_ = new QVBoxLayout(this->expandedWidget_);
    this->expandedLayout_->setContentsMargins(8, 0, 8, 6);
    this->expandedLayout_->setSpacing(2);

    this->expandedQuestionLabel_ = new QLabel(this->expandedWidget_);
    this->expandedQuestionLabel_->setWordWrap(true);
    this->expandedQuestionLabel_->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    this->pollStatusLabel_ = new QLabel(this->expandedWidget_);
    this->pollStatusLabel_->setWordWrap(false);
    this->pollStatusLabel_->hide();

    this->countdownLabel_ = new QLabel(this->expandedWidget_);
    this->countdownLabel_->setWordWrap(false);

    this->choicesContainer_ = new QWidget(this->expandedWidget_);
    this->choicesLayout_ = new QVBoxLayout(this->choicesContainer_);
    this->choicesLayout_->setContentsMargins(0, 0, 0, 0);
    this->choicesLayout_->setSpacing(3);

    this->expandedLayout_->addWidget(this->expandedQuestionLabel_);
    this->expandedLayout_->addWidget(this->pollStatusLabel_);
    this->expandedLayout_->addWidget(this->countdownLabel_);
    this->expandedLayout_->addWidget(this->choicesContainer_);

    mainLayout->addWidget(topRow);
    mainLayout->addWidget(this->expandedWidget_);

    this->installClickFocusesSplit(this);

    this->updateExpandToggle();

    getSettings()->showPollPanel.connect(
        [this](const bool &enabled) {
            this->startOrStopTimer();
            if (!enabled)
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

    this->themeChangedEvent();
    this->scaleChangedEvent(this->scale());
}

void SplitPollPanel::installClickFocusesSplit(QWidget *root)
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

bool SplitPollPanel::eventFilter(QObject *watched, QEvent *event)
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

void SplitPollPanel::setTwitchChannel(TwitchChannel *channel)
{
    this->channelConnections_.clear();
    this->twitchChannel_ = channel;
    this->pollTimer_.stop();
    this->countdownTimer_.stop();
    this->lingerTimer_.stop();
    this->inFlight_ = false;
    this->voteInFlight_ = false;
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

void SplitPollPanel::refresh()
{
    this->fetchPoll();
}

void SplitPollPanel::recoverDismissedPanel()
{
    this->dismissedForPollId_.clear();
    this->refresh();
}

void SplitPollPanel::startOrStopTimer()
{
    this->pollTimer_.stop();
    this->countdownTimer_.stop();

    if (!getSettings()->showPollPanel)
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

void SplitPollPanel::fetchPoll()
{
    if (!getSettings()->showPollPanel)
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
    const QString fetchLogin = tc->getName();
    const auto acc = getApp()->getAccounts()->twitch.getCurrent();

    TwitchGql::fetchViewablePollForChannel(
        fetchRoomId, fetchLogin, acc->getOAuthClient(), acc->getOAuthToken(),
        this,
        [this, fetchRoomId](std::optional<TwitchGql::ViewablePoll> poll) {
            this->inFlight_ = false;
            if (this->twitchChannel_ == nullptr ||
                this->twitchChannel_->roomId() != fetchRoomId)
            {
                return;
            }

            if (!poll.has_value())
            {
                if (this->lingering_)
                {
                    return;
                }
                if (this->lastLivePoll_.has_value() &&
                    !this->lastLivePoll_->id.isEmpty())
                {
                    if (!this->dismissedForPollId_.isEmpty() &&
                        this->dismissedForPollId_ == this->lastLivePoll_->id)
                    {
                        this->hidePollUi();
                        return;
                    }
                    this->lingerEndedDisplay_ = true;
                    this->current_ = *this->lastLivePoll_;
                    this->lingering_ = true;
                    this->lingerPollId_ = this->lastLivePoll_->id;
                    this->currentDisplayPollId_ = this->lastLivePoll_->id;
                    this->applyPollToUi(*this->current_);
                    this->show();
                    this->updateStyleSheets();
                    if (!this->lingerTimer_.isActive())
                    {
                        this->lingerTimer_.start();
                    }
                    return;
                }
                this->hidePollUi();
                return;
            }

            TwitchGql::ViewablePoll p = std::move(*poll);
            if (p.id.isEmpty())
            {
                this->hidePollUi();
                return;
            }

            this->lingerEndedDisplay_ = false;

            if (!this->dismissedForPollId_.isEmpty() &&
                p.id == this->dismissedForPollId_)
            {
                this->current_.reset();
                this->hidePollUi();
                return;
            }

            if (!this->currentDisplayPollId_.isEmpty() &&
                p.id != this->currentDisplayPollId_)
            {
                this->lingerTimer_.stop();
                this->lingering_ = false;
                this->lingerPollId_.clear();
            }

            if (TwitchGql::pollStatusIsTerminal(p.status))
            {
                TwitchGql::ViewablePoll merged = mergePollWithPriorViewerVotes(
                    std::move(p), this->lastLivePoll_);
                this->lastLivePoll_ = merged;
                this->current_ = merged;
                this->currentDisplayPollId_ = merged.id;
                const bool needTimerStart = !this->lingerTimer_.isActive() ||
                                            this->lingerPollId_ != merged.id;
                this->lingering_ = true;
                this->lingerPollId_ = merged.id;
                this->applyPollToUi(merged);
                this->show();
                this->updateStyleSheets();
                if (needTimerStart)
                {
                    this->lingerTimer_.start();
                }
                return;
            }

            this->lingerTimer_.stop();
            this->lingering_ = false;
            this->lingerPollId_.clear();

            TwitchGql::ViewablePoll merged = mergePollWithPriorViewerVotes(
                std::move(p), this->lastLivePoll_);
            this->lastLivePoll_ = merged;
            this->current_ = merged;
            this->currentDisplayPollId_ = merged.id;
            this->applyPollToUi(merged);
            this->show();
            this->updateStyleSheets();
            if (!this->countdownTimer_.isActive())
            {
                this->countdownTimer_.start();
            }
        },
        [this, fetchRoomId](const QString &) {
            this->inFlight_ = false;
            if (this->twitchChannel_ != nullptr &&
                this->twitchChannel_->roomId() == fetchRoomId)
            {
                this->hidePollUi();
            }
        });
}

void SplitPollPanel::applyPollToUi(const TwitchGql::ViewablePoll &poll)
{
    this->lastTitleForElide_ = poll.title.trimmed().isEmpty()
                                   ? QStringLiteral("Poll")
                                   : poll.title.trimmed();

    if (this->expandedQuestionLabel_ != nullptr)
    {
        this->expandedQuestionLabel_->setText(this->lastTitleForElide_);
    }

    const bool terminal = TwitchGql::pollStatusIsTerminal(poll.status);
    const bool endedUi = terminal || this->lingerEndedDisplay_;
    if (this->pollStatusLabel_ != nullptr)
    {
        if (endedUi)
        {
            this->pollStatusLabel_->setVisible(true);
            this->pollStatusLabel_->setText(QStringLiteral("Poll Ended"));
        }
        else
        {
            this->pollStatusLabel_->hide();
            this->pollStatusLabel_->clear();
        }
    }

    if (endedUi)
    {
        this->countdownTimer_.stop();
        if (this->countdownLabel_ != nullptr)
        {
            this->countdownLabel_->clear();
            this->countdownLabel_->hide();
        }
    }
    else
    {
        this->displayRemainingMs_ =
            std::max(qint64{0}, poll.remainingDurationMs);
        if (this->countdownLabel_ != nullptr)
        {
            this->countdownLabel_->show();
            if (!this->voteError_.isEmpty())
            {
                this->countdownLabel_->setText(this->voteError_);
            }
            else
            {
                this->countdownLabel_->setText(
                    formatCountdownMs(this->displayRemainingMs_));
            }
        }
    }

    this->rebuildChoiceRows();
    this->refreshTopRowText();
    this->updatePollIcon();
}

void SplitPollPanel::castVote(const QString &choiceId)
{
    if (this->voteInFlight_ || !this->current_.has_value())
    {
        return;
    }
    if (!this->current_->viewerVotedChoiceIds.empty())
    {
        return;
    }
    if (!TwitchGql::pollStatusIsActive(this->current_->status))
    {
        return;
    }
    const auto acc = getApp()->getAccounts()->twitch.getCurrent();
    if (acc->isAnon())
    {
        return;
    }
    const QString uid = acc->getUserId();
    if (uid.isEmpty())
    {
        this->voteError_ = QStringLiteral("Sign in to vote");
        if (this->countdownLabel_ != nullptr)
        {
            this->countdownLabel_->show();
            this->countdownLabel_->setText(this->voteError_);
        }
        return;
    }

    this->voteError_.clear();
    this->voteInFlight_ = true;
    this->rebuildChoiceRows();

    TwitchGql::voteInPoll(
        this->current_->id, choiceId, uid, acc->getOAuthToken(),
        acc->getOAuthClient(), this,
        [this, choiceId] {
            this->voteInFlight_ = false;
            this->voteError_.clear();
            if (this->current_.has_value())
            {
                auto &ids = this->current_->viewerVotedChoiceIds;
                if (std::find(ids.begin(), ids.end(), choiceId) == ids.end())
                {
                    ids.push_back(choiceId);
                }
                this->lastLivePoll_ = *this->current_;
            }
            this->rebuildChoiceRows();
            this->refresh();
        },
        [this](QString err) {
            this->voteInFlight_ = false;
            this->voteError_ =
                err.isEmpty() ? QStringLiteral("Vote failed") : std::move(err);
            if (this->countdownLabel_ != nullptr &&
                TwitchGql::pollStatusIsActive(this->current_->status))
            {
                this->countdownLabel_->show();
                this->countdownLabel_->setText(this->voteError_);
            }
            this->rebuildChoiceRows();
        });
}

void SplitPollPanel::onLingerTimeout()
{
    this->lastLivePoll_.reset();
    this->lingerEndedDisplay_ = false;
    this->lingering_ = false;
    this->lingerPollId_.clear();
    this->currentDisplayPollId_.clear();
    this->hidePollUi();
}

void SplitPollPanel::rebuildChoiceRows()
{
    if (this->choicesLayout_ == nullptr)
    {
        return;
    }

    auto clearRows = [this] {
        for (auto *row : this->pollRows_)
        {
            if (row != nullptr)
            {
                this->choicesLayout_->removeWidget(row);
                row->deleteLater();
            }
        }
        this->pollRows_.clear();
    };

    if (!this->current_.has_value())
    {
        clearRows();
        return;
    }

    const auto &poll = *this->current_;
    const bool terminal = TwitchGql::pollStatusIsTerminal(poll.status);
    const bool endedUi = terminal || this->lingerEndedDisplay_;
    const bool active = TwitchGql::pollStatusIsActive(poll.status) &&
                        !this->lingerEndedDisplay_;

    QString winId = poll.winningChoiceId;
    if (endedUi && winId.isEmpty())
    {
        winId = winningChoiceIdFromVotes(poll);
    }

    int sumVotes = 0;
    for (const auto &c : poll.choices)
    {
        sumVotes += std::max(0, c.votesTotal);
    }
    sumVotes = std::max(1, sumVotes);

    const bool viewerHasVote = !poll.viewerVotedChoiceIds.empty();

    const bool reuseWidgets = pollRowsMatchPoll(this->pollRows_, poll);
    if (!reuseWidgets)
    {
        clearRows();
    }

    const auto configureOne = [&](PollChoiceRow *row,
                                  const TwitchGql::ViewablePollChoice &ch) {
        const int v = std::max(0, ch.votesTotal);
        const double fill =
            static_cast<double>(v) / static_cast<double>(sumVotes);
        const int pct = static_cast<int>(std::lround(100.0 * fill));
        const bool isWinner = endedUi && !winId.isEmpty() && ch.id == winId;
        const bool votedThis =
            std::find(poll.viewerVotedChoiceIds.begin(),
                      poll.viewerVotedChoiceIds.end(),
                      ch.id) != poll.viewerVotedChoiceIds.end();
        const bool canVote = active && !this->voteInFlight_ && !viewerHasVote;
        const bool dimOthers = active && viewerHasVote && !votedThis;
        row->configure(this->theme, this->scale(), ch.id, ch.title, fill, v,
                       pct, endedUi, isWinner, canVote, dimOthers,
                       active && votedThis);
    };

    if (reuseWidgets)
    {
        for (size_t i = 0; i < poll.choices.size(); ++i)
        {
            configureOne(this->pollRows_[i], poll.choices[i]);
        }
        return;
    }

    for (const auto &ch : poll.choices)
    {
        auto *row = new PollChoiceRow(this->choicesContainer_);
        configureOne(row, ch);
        row->setPickHandler([this](const QString &id) {
            this->castVote(id);
        });
        this->choicesLayout_->addWidget(row);
        this->pollRows_.push_back(row);
        this->installClickFocusesSplit(row);
    }
}

void SplitPollPanel::tickCountdownDisplay()
{
    if (!this->current_.has_value() || !this->isVisible())
    {
        this->countdownTimer_.stop();
        return;
    }
    if (TwitchGql::pollStatusIsTerminal(this->current_->status) ||
        this->lingerEndedDisplay_)
    {
        this->countdownTimer_.stop();
        return;
    }
    this->displayRemainingMs_ =
        std::max(qint64{0}, this->displayRemainingMs_ - 1'000);
    if (this->countdownLabel_ != nullptr)
    {
        this->countdownLabel_->show();
        if (!this->voteError_.isEmpty())
        {
            this->countdownLabel_->setText(this->voteError_);
        }
        else
        {
            this->countdownLabel_->setText(
                formatCountdownMs(this->displayRemainingMs_));
        }
    }
}

void SplitPollPanel::hidePollUi()
{
    this->lingerTimer_.stop();
    this->lingering_ = false;
    this->lingerPollId_.clear();
    this->currentDisplayPollId_.clear();
    this->lingerEndedDisplay_ = false;
    this->voteInFlight_ = false;

    this->expanded_ = true;
    this->updateExpandToggle();
    this->current_.reset();
    this->lastTitleForElide_.clear();
    this->displayRemainingMs_ = 0;
    this->voteError_.clear();
    this->countdownTimer_.stop();
    if (this->collapsedTitle_ != nullptr)
    {
        this->collapsedTitle_->clear();
    }
    if (this->expandedQuestionLabel_ != nullptr)
    {
        this->expandedQuestionLabel_->clear();
    }
    if (this->pollStatusLabel_ != nullptr)
    {
        this->pollStatusLabel_->hide();
        this->pollStatusLabel_->clear();
    }
    if (this->countdownLabel_ != nullptr)
    {
        this->countdownLabel_->clear();
        this->countdownLabel_->hide();
    }
    this->rebuildChoiceRows();
    this->hide();
}

void SplitPollPanel::hidePanel()
{
    this->dismissedForPollId_.clear();
    this->lastLivePoll_.reset();
    this->voteInFlight_ = false;
    this->hidePollUi();
}

void SplitPollPanel::updatePollIcon()
{
    if (this->iconLabel_ == nullptr || this->theme == nullptr)
    {
        return;
    }

    const int px = splitHeaderIconColumnWidth(this->scale());
    const QString iconPath =
        this->theme->isLightTheme()
            ? QStringLiteral(":/buttons/bullet-list-lightMode.svg")
            : QStringLiteral(":/buttons/bullet-list-darkMode.svg");
    const QPixmap pm = QIcon(iconPath).pixmap(px, px);
    if (!pm.isNull())
    {
        this->iconLabel_->setPixmap(pm);
    }
    this->iconLabel_->setFixedWidth(px);
    this->iconLabel_->setToolTip(QStringLiteral("Channel poll"));
}

void SplitPollPanel::updateExpandToggle()
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
}

void SplitPollPanel::refreshTopRowText()
{
    if (this->collapsedTitle_ == nullptr || this->lastTitleForElide_.isEmpty())
    {
        return;
    }

    if (this->expanded_)
    {
        this->collapsedTitle_->setText(QStringLiteral("Poll"));
        return;
    }

    this->updateCollapsedElide();
}

void SplitPollPanel::updateCollapsedElide()
{
    if (this->collapsedTitle_ == nullptr ||
        this->lastTitleForElide_.isEmpty() || this->expanded_)
    {
        return;
    }

    int avail = 0;
    auto *topRow = this->collapsedTitle_->parentWidget();
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
        avail = this->collapsedTitle_->width();
    }

    avail = std::max(40, avail);

    const bool ended =
        this->current_.has_value() &&
        (TwitchGql::pollStatusIsTerminal(this->current_->status) ||
         this->lingerEndedDisplay_);
    const QString line =
        ended ? QStringLiteral("Poll ended")
              : QStringLiteral("Poll: %1").arg(this->lastTitleForElide_);
    const QFontMetrics fm(this->collapsedTitle_->font());
    const QString elided = fm.elidedText(line, Qt::ElideRight, avail);
    this->collapsedTitle_->setText(elided.isEmpty() ? line : elided);
}

void SplitPollPanel::updateStyleSheets()
{
    if (this->theme == nullptr)
    {
        return;
    }

    const auto &text = this->theme->splits.header.text;
    const auto textCss = text.name(QColor::HexArgb);

    const QString labelStyle =
        QStringLiteral("QLabel { color: %1; }").arg(textCss);

    for (auto *w : {this->collapsedTitle_, this->expandedQuestionLabel_,
                    this->pollStatusLabel_, this->countdownLabel_})
    {
        if (w != nullptr)
        {
            w->setStyleSheet(labelStyle);
        }
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

    for (auto *row : this->pollRows_)
    {
        if (row != nullptr)
        {
            row->update();
        }
    }
}

void SplitPollPanel::themeChangedEvent()
{
    BaseWidget::themeChangedEvent();
    this->updateStyleSheets();
    this->updatePollIcon();
    if (this->current_.has_value())
    {
        this->rebuildChoiceRows();
    }
}

void SplitPollPanel::scaleChangedEvent(float scale)
{
    BaseWidget::scaleChangedEvent(scale);

    const int fs = static_cast<int>(9 * std::max(1.0f, scale));
    QFont f = this->font();
    f.setPointSize(std::max(8, fs));
    f.setBold(true);
    if (this->collapsedTitle_ != nullptr)
    {
        this->collapsedTitle_->setFont(f);
    }
    if (this->expandedQuestionLabel_ != nullptr)
    {
        QFont fq = f;
        fq.setBold(true);
        this->expandedQuestionLabel_->setFont(fq);
    }
    if (this->pollStatusLabel_ != nullptr)
    {
        QFont fsz = f;
        fsz.setBold(true);
        this->pollStatusLabel_->setFont(fsz);
    }
    if (this->countdownLabel_ != nullptr)
    {
        QFont fc = f;
        fc.setBold(false);
        this->countdownLabel_->setFont(fc);
    }

    this->updatePollIcon();
    if (auto *lay = this->expandedWidget_ != nullptr
                        ? this->expandedWidget_->layout()
                        : nullptr)
    {
        const int pad = static_cast<int>(4 * scale);
        lay->setContentsMargins(8, pad, 8, pad + 2);
    }

    this->updateCollapsedElide();
    if (this->current_.has_value())
    {
        this->rebuildChoiceRows();
    }
}

void SplitPollPanel::paintEvent(QPaintEvent * /*event*/)
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

void SplitPollPanel::resizeEvent(QResizeEvent *event)
{
    BaseWidget::resizeEvent(event);
    this->updateCollapsedElide();
}

void SplitPollPanel::showEvent(QShowEvent *event)
{
    BaseWidget::showEvent(event);
    QTimer::singleShot(0, this, [this] {
        this->updateCollapsedElide();
    });
}

}  // namespace chatterino

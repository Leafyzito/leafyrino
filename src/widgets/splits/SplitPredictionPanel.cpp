// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/SplitPredictionPanel.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "widgets/splits/PredictionPoolBar.hpp"
#include "widgets/splits/Split.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QColor>
#include <QDateTime>
#include <QDesktopServices>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace {

using namespace chatterino;

constexpr int POLL_INTERVAL_MS = 10'000;
constexpr int LINGER_MS = 30'000;

bool isResolvedLike(const HelixPrediction &p)
{
    if (!p.winningOutcomeID.isEmpty() || !p.winningOutcomeTitle.isEmpty())
    {
        return true;
    }
    return p.status.compare(QStringLiteral("RESOLVED"), Qt::CaseInsensitive) ==
               0 ||
           p.status.compare(QStringLiteral("RESOLVE_PENDING"),
                            Qt::CaseInsensitive) == 0;
}

/// -1 = none, 0 = left outcome, 1 = right outcome (binary UI).
int winnerSide(const HelixPrediction &p)
{
    if (p.outcomes.empty())
    {
        return -1;
    }
    const int n = std::min(2, static_cast<int>(p.outcomes.size()));

    if (!p.winningOutcomeID.isEmpty())
    {
        const QString wid = p.winningOutcomeID.trimmed();
        for (int i = 0; i < n; ++i)
        {
            if (p.outcomes[static_cast<size_t>(i)].id.trimmed() == wid)
            {
                return i;
            }
        }
    }

    if (!p.winningOutcomeTitle.isEmpty())
    {
        const QString wt = p.winningOutcomeTitle.trimmed();
        for (int i = 0; i < n; ++i)
        {
            if (p.outcomes[static_cast<size_t>(i)].title.compare(
                    wt, Qt::CaseInsensitive) == 0)
            {
                return i;
            }
        }
    }

    return -1;
}

QString winnerDisplayTitle(const HelixPrediction &p, int wSide)
{
    if (wSide >= 0 && wSide < static_cast<int>(p.outcomes.size()))
    {
        return p.outcomes[static_cast<size_t>(wSide)].title;
    }
    if (!p.winningOutcomeTitle.isEmpty())
    {
        return p.winningOutcomeTitle.trimmed();
    }
    const QString wid = p.winningOutcomeID.trimmed();
    for (const auto &o : p.outcomes)
    {
        if (o.id.trimmed() == wid)
        {
            return o.title;
        }
    }
    return {};
}

QDateTime parseCreatedAtUtc(const QString &s)
{
    QString t = s.trimmed();
    if (t.isEmpty())
    {
        return {};
    }
    QDateTime dt = QDateTime::fromString(t, Qt::ISODate);
    if (dt.isValid())
    {
        return dt.toUTC();
    }
    const int dot = t.indexOf('.');
    const int z = t.lastIndexOf(QLatin1Char('Z'));
    if (dot >= 0 && z > dot)
    {
        const QString frac = t.mid(dot + 1, z - dot - 1);
        if (frac.length() > 3)
        {
            t = t.left(dot + 1) + frac.left(3) + QLatin1Char('Z');
            dt = QDateTime::fromString(t, Qt::ISODateWithMs);
            if (dt.isValid())
            {
                return dt.toUTC();
            }
        }
    }
    return {};
}

QString formatCountdown(qint64 totalSecs)
{
    if (totalSecs < 0)
    {
        totalSecs = 0;
    }
    const qint64 h = totalSecs / 3600;
    const qint64 m = (totalSecs % 3600) / 60;
    const qint64 s = totalSecs % 60;
    if (h > 0)
    {
        return QStringLiteral("%1:%2:%3")
            .arg(h)
            .arg(m, 2, 10, QLatin1Char('0'))
            .arg(s, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

}  // namespace

namespace chatterino {

SplitPredictionPanel::SplitPredictionPanel(Split *split)
    : BaseWidget(split)
    , split_(split)
{
    this->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Minimum);
    this->hide();

    this->pollTimer_.setInterval(POLL_INTERVAL_MS);
    this->pollTimer_.setTimerType(Qt::CoarseTimer);
    QObject::connect(&this->pollTimer_, &QTimer::timeout, this, [this] {
        this->fetchPredictions();
    });

    this->countdownTimer_.setInterval(1000);
    this->countdownTimer_.setTimerType(Qt::CoarseTimer);
    QObject::connect(&this->countdownTimer_, &QTimer::timeout, this, [this] {
        this->tickPredictionCountdown();
    });

    this->lingerTimer_.setInterval(LINGER_MS);
    this->lingerTimer_.setSingleShot(true);
    this->lingerTimer_.setTimerType(Qt::CoarseTimer);
    QObject::connect(&this->lingerTimer_, &QTimer::timeout, this, [this] {
        this->hidePanel();
    });

    this->pointsPollTimer_.setInterval(POLL_INTERVAL_MS);
    this->pointsPollTimer_.setTimerType(Qt::CoarseTimer);
    QObject::connect(&this->pointsPollTimer_, &QTimer::timeout, this, [this] {
        this->fetchChannelPoints();
    });

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    auto *topRow = new QWidget(this);
    auto *topLayout = new QHBoxLayout(topRow);
    topLayout->setContentsMargins(6, 2, 6, 2);
    topLayout->setSpacing(4);

    this->collapsedTitle_ = new QLabel(topRow);
    this->collapsedTitle_->setSizePolicy(QSizePolicy::MinimumExpanding,
                                         QSizePolicy::Preferred);
    this->collapsedTitle_->setWordWrap(false);

    this->expandButton_ = new QPushButton(topRow);
    this->expandButton_->setFlat(true);
    this->expandButton_->setCursor(Qt::PointingHandCursor);
    this->expandButton_->setFocusPolicy(Qt::NoFocus);
    QObject::connect(this->expandButton_, &QPushButton::clicked, this, [this] {
        this->expanded_ = !this->expanded_;
        this->updateExpandToggle();
    });

    this->openTwitchButton_ =
        new QPushButton(QStringLiteral("Open on Twitch"), topRow);
    this->openTwitchButton_->setFlat(true);
    this->openTwitchButton_->setCursor(Qt::PointingHandCursor);
    this->openTwitchButton_->setFocusPolicy(Qt::NoFocus);
    QObject::connect(this->openTwitchButton_, &QPushButton::clicked, this,
                     [this] {
                         this->openPopoutChat();
                     });

    topLayout->addWidget(this->collapsedTitle_, 1);
    topLayout->addWidget(this->expandButton_, 0, Qt::AlignRight);
    topLayout->addWidget(this->openTwitchButton_, 0, Qt::AlignRight);

    this->expandedWidget_ = new QWidget(this);
    this->expandedLayout_ = new QVBoxLayout(this->expandedWidget_);
    this->expandedLayout_->setContentsMargins(8, 0, 8, 6);
    this->expandedLayout_->setSpacing(4);

    this->fullTitle_ = new QLabel(this->expandedWidget_);
    this->fullTitle_->setWordWrap(true);

    this->statusLabel_ = new QLabel(this->expandedWidget_);

    this->outcomeRow_ = new QWidget(this->expandedWidget_);
    {
        auto *hl = new QHBoxLayout(this->outcomeRow_);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(4);

        auto *leftWrap = new QWidget(this->outcomeRow_);
        auto *leftLay = new QHBoxLayout(leftWrap);
        leftLay->setContentsMargins(0, 0, 0, 0);
        leftLay->setSpacing(4);
        this->outcomeTitle0_ = new QLabel(leftWrap);
        this->outcomeDash0_ = new QLabel(QStringLiteral(" - "), leftWrap);
        this->outcomePct0_ = new QLabel(leftWrap);
        this->outcomeWon0_ = new QLabel(QStringLiteral("Won"), leftWrap);
        leftLay->addWidget(this->outcomeTitle0_, 0);
        leftLay->addWidget(this->outcomeDash0_, 0);
        leftLay->addWidget(this->outcomePct0_, 0);
        leftLay->addWidget(this->outcomeWon0_, 0);

        auto *rightWrap = new QWidget(this->outcomeRow_);
        auto *rightLay = new QHBoxLayout(rightWrap);
        rightLay->setContentsMargins(0, 0, 0, 0);
        rightLay->setSpacing(4);
        this->outcomeWon1_ = new QLabel(QStringLiteral("Won"), rightWrap);
        this->outcomePct1_ = new QLabel(rightWrap);
        this->outcomeDash1_ = new QLabel(QStringLiteral(" - "), rightWrap);
        this->outcomeTitle1_ = new QLabel(rightWrap);
        rightLay->addWidget(this->outcomeWon1_, 0);
        rightLay->addWidget(this->outcomePct1_, 0);
        rightLay->addWidget(this->outcomeDash1_, 0);
        rightLay->addWidget(this->outcomeTitle1_, 0);

        this->outcomeWon0_->hide();
        this->outcomeWon1_->hide();

        hl->addWidget(leftWrap, 0);
        hl->addStretch(1);
        hl->addWidget(rightWrap, 0);
    }

    this->expandedLayout_->addWidget(this->fullTitle_);
    this->expandedLayout_->addWidget(this->statusLabel_);
    this->expandedLayout_->addWidget(this->outcomeRow_);

    this->poolBar_ = new PredictionPoolBar(this->expandedWidget_);
    this->expandedLayout_->addWidget(this->poolBar_);

    auto *pointsRow = new QWidget(this->expandedWidget_);
    auto *pointsLay = new QHBoxLayout(pointsRow);
    pointsLay->setContentsMargins(0, 0, 0, 0);
    pointsLay->setSpacing(6);
    this->yourPointsLabel_ =
        new QLabel(QStringLiteral("Your points:"), pointsRow);
    this->yourPointsValue_ = new QLabel(QStringLiteral("\u2014"), pointsRow);
    pointsLay->addWidget(this->yourPointsLabel_, 0);
    pointsLay->addWidget(this->yourPointsValue_, 0);
    pointsLay->addStretch(1);
    this->expandedLayout_->addWidget(pointsRow);

    this->expandedLayout_->addSpacing(6);
    this->disclaimerLabel_ = new QLabel(
        QStringLiteral("Channel points cannot be used in Chatterino yet. For "
                       "that, press the 'Open on Twitch' button."),
        this->expandedWidget_);
    this->disclaimerLabel_->setWordWrap(true);
    this->disclaimerLabel_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    this->expandedLayout_->addWidget(this->disclaimerLabel_);

    mainLayout->addWidget(topRow);
    mainLayout->addWidget(this->expandedWidget_);

    getSettings()->showPredictionPanel.connect(
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

    this->boostConnections_.emplace_back(
        getApp()->getAccounts()->twitch.currentUserChanged.connect([this] {
            this->startOrStopTimer();
            this->refresh();
        }));

    this->managedConnections_.addConnection(pajlada::Signals::ScopedConnection(
        this->split_->focused.connect([this] {
            this->update();
            this->updateStyleSheets();
        })));
    this->managedConnections_.addConnection(pajlada::Signals::ScopedConnection(
        this->split_->focusLost.connect([this] {
            this->update();
            this->updateStyleSheets();
        })));

    this->themeChangedEvent();
    this->scaleChangedEvent(this->scale());
    this->updateExpandToggle();
}

void SplitPredictionPanel::setTwitchChannel(TwitchChannel *channel)
{
    this->channelConnections_.clear();
    this->twitchChannel_ = channel;
    this->pollTimer_.stop();
    this->countdownTimer_.stop();
    this->pointsPollTimer_.stop();
    this->predictionBettingEnds_ = QDateTime();
    this->inFlight_ = false;
    this->hidePanel();

    if (channel == nullptr)
    {
        return;
    }

    this->channelConnections_.managedConnect(channel->roomIdAssigned, [this] {
        this->refresh();
    });
    this->channelConnections_.managedConnect(channel->destroyed, [this] {
        this->setTwitchChannel(nullptr);
    });

    this->startOrStopTimer();
    this->refresh();
}

void SplitPredictionPanel::refresh()
{
    this->fetchPredictions();
}

void SplitPredictionPanel::startOrStopTimer()
{
    this->pollTimer_.stop();

    if (!getSettings()->showPredictionPanel)
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

void SplitPredictionPanel::fetchPredictions()
{
    if (!getSettings()->showPredictionPanel)
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

    TwitchGql::fetchPredictionsForChannel(
        fetchRoomId, tc->getName(), acc->getOAuthClient(), acc->getOAuthToken(),
        this,
        [this, fetchRoomId](std::optional<HelixPrediction> picked) {
            this->inFlight_ = false;

            if (this->twitchChannel_ == nullptr ||
                this->twitchChannel_->roomId() != fetchRoomId)
            {
                return;
            }

            if (!picked.has_value())
            {
                if (this->lingering_)
                {
                    return;
                }
                if (this->lastLivePrediction_.has_value() &&
                    !this->lastLivePrediction_->id.isEmpty())
                {
                    this->currentDisplayId_ = this->lastLivePrediction_->id;
                    this->lingering_ = true;
                    this->lingerEventId_ = this->lastLivePrediction_->id;
                    this->renderPrediction(*this->lastLivePrediction_, false);
                    this->show();
                    this->updateExpandToggle();
                    this->updateStyleSheets();
                    this->onPanelShown();
                    if (!this->lingerTimer_.isActive())
                    {
                        this->lingerTimer_.start();
                    }
                }
                else
                {
                    this->hidePanel();
                }
                return;
            }

            const HelixPrediction &pr = *picked;
            if (pr.id.isEmpty())
            {
                this->hidePanel();
                return;
            }

            if (!this->currentDisplayId_.isEmpty() &&
                pr.id != this->currentDisplayId_)
            {
                this->lingerTimer_.stop();
                this->lingering_ = false;
                this->lingerEventId_.clear();
            }

            if (isResolvedLike(pr))
            {
                this->currentDisplayId_ = pr.id;
                this->renderPrediction(pr, false);
                this->show();
                this->updateExpandToggle();
                this->updateStyleSheets();
                this->onPanelShown();
                const bool needTimerStart =
                    !this->lingering_ || this->lingerEventId_ != pr.id;
                this->lingering_ = true;
                this->lingerEventId_ = pr.id;
                if (needTimerStart)
                {
                    this->lingerTimer_.start();
                }
                return;
            }

            this->lingerTimer_.stop();
            this->lingering_ = false;
            this->lingerEventId_.clear();
            this->lastLivePrediction_ = pr;
            this->currentDisplayId_ = pr.id;
            this->renderPrediction(pr, true);
            this->show();
            this->updateExpandToggle();
            this->updateStyleSheets();
            this->onPanelShown();
        },
        [this, fetchRoomId](const QString & /*err*/) {
            this->inFlight_ = false;

            if (this->twitchChannel_ != nullptr &&
                this->twitchChannel_->roomId() == fetchRoomId)
            {
                this->hidePanel();
            }
        });
}

void SplitPredictionPanel::onPanelShown()
{
    this->pointsPollTimer_.stop();
    this->pointsPollTimer_.setInterval(POLL_INTERVAL_MS);
    this->pointsPollTimer_.start();
    this->fetchChannelPoints();
}

void SplitPredictionPanel::fetchChannelPoints()
{
    if (!this->isVisible())
    {
        return;
    }
    if (!getSettings()->showPredictionPanel)
    {
        return;
    }

    auto *tc = this->twitchChannel_;
    if (tc == nullptr || this->pointsInFlight_)
    {
        return;
    }
    if (getApp()->getAccounts()->twitch.getCurrent()->isAnon())
    {
        return;
    }

    const QString login = tc->getName().trimmed();
    if (login.isEmpty() || login.startsWith(QLatin1Char('/')))
    {
        return;
    }

    this->pointsInFlight_ = true;
    const QString fetchLogin = login;
    const auto acc = getApp()->getAccounts()->twitch.getCurrent();

    TwitchGql::fetchChannelPointsBalance(
        fetchLogin, acc->getOAuthToken(), this,
        [this, fetchLogin](int balance) {
            this->pointsInFlight_ = false;
            if (!this->isVisible() || this->twitchChannel_ == nullptr ||
                this->twitchChannel_->getName().trimmed().compare(
                    fetchLogin, Qt::CaseInsensitive) != 0)
            {
                return;
            }
            if (this->yourPointsValue_ != nullptr)
            {
                this->yourPointsValue_->setText(QString::number(balance));
            }
        },
        [this, fetchLogin](const QString &) {
            this->pointsInFlight_ = false;
            if (!this->isVisible() || this->twitchChannel_ == nullptr ||
                this->twitchChannel_->getName().trimmed().compare(
                    fetchLogin, Qt::CaseInsensitive) != 0)
            {
                return;
            }
            if (this->yourPointsValue_ != nullptr)
            {
                this->yourPointsValue_->setText(QStringLiteral("\u2014"));
            }
        });
}

void SplitPredictionPanel::renderPrediction(const HelixPrediction &prediction,
                                            bool liveMode)
{
    if (!getSettings()->showPredictionPanel)
    {
        this->hidePanel();
        return;
    }

    const auto &outcomes = prediction.outcomes;
    if (outcomes.empty())
    {
        this->hidePanel();
        return;
    }

    const QString collapsedTitled =
        QStringLiteral("Prediction: %1").arg(prediction.title);
    this->fullTitle_->setText(prediction.title);

    this->countdownTimer_.stop();
    this->predictionBettingEnds_ = QDateTime();

    const int wSide = liveMode ? -1 : winnerSide(prediction);

    QString statusText;
    if (liveMode)
    {
        if (prediction.status.compare(QStringLiteral("LOCKED"),
                                      Qt::CaseInsensitive) == 0)
        {
            statusText = QStringLiteral("Locked - waiting for outcome");
        }
        else
        {
            statusText = QStringLiteral("Active");
            if (prediction.predictionWindow > 0 &&
                !prediction.createdAt.isEmpty())
            {
                const QDateTime started =
                    parseCreatedAtUtc(prediction.createdAt);
                if (started.isValid())
                {
                    this->predictionBettingEnds_ =
                        started.addSecs(prediction.predictionWindow);
                    const auto now = QDateTime::currentDateTimeUtc();
                    const qint64 secs =
                        now.secsTo(this->predictionBettingEnds_);
                    if (secs > 0)
                    {
                        statusText = QStringLiteral("Active - closes in %1")
                                         .arg(formatCountdown(secs));
                        this->countdownTimer_.start();
                    }
                    else
                    {
                        statusText =
                            QStringLiteral("Active - betting window ended");
                        this->predictionBettingEnds_ = QDateTime();
                    }
                }
            }
        }
    }
    else
    {
        const QString wname = winnerDisplayTitle(prediction, wSide);
        if (!wname.isEmpty())
        {
            statusText = QStringLiteral("Winner: %1").arg(wname);
        }
        else if (!prediction.winningOutcomeID.isEmpty() ||
                 !prediction.winningOutcomeTitle.isEmpty() ||
                 prediction.status.compare(QStringLiteral("RESOLVED"),
                                           Qt::CaseInsensitive) == 0 ||
                 prediction.status.compare(QStringLiteral("RESOLVE_PENDING"),
                                           Qt::CaseInsensitive) == 0)
        {
            statusText = QStringLiteral("Resolved");
        }
        else
        {
            statusText = QStringLiteral("Prediction ended");
        }
    }

    this->statusLabel_->setText(statusText);

    const HelixPredictionOutcome *o0 = &outcomes[0];
    const HelixPredictionOutcome *o1 =
        outcomes.size() > 1 ? &outcomes[1] : nullptr;

    int p0 = std::max(0, o0->channelPoints);
    int p1 = o1 ? std::max(0, o1->channelPoints) : 0;
    const int total = p0 + p1;
    int pct0 = 50;
    int pct1 = 50;
    if (total > 0)
    {
        pct0 = static_cast<int>((100LL * p0 + total / 2) / total);
        pct1 = 100 - pct0;
    }

    this->outcomeTitle0_->setText(o0->title);
    this->outcomePct0_->setText(QStringLiteral("%1%").arg(pct0));

    this->outcomeWon0_->setVisible(wSide == 0);
    this->outcomeWon1_->setVisible(wSide == 1);

    if (o1 != nullptr)
    {
        this->outcomeTitle1_->show();
        this->outcomeDash1_->show();
        this->outcomePct1_->show();
        this->outcomeTitle1_->setText(o1->title);
        this->outcomePct1_->setText(QStringLiteral("%1%").arg(pct1));
        const double leftFrac =
            total > 0 ? static_cast<double>(p0) / static_cast<double>(total)
                      : 0.5;
        this->poolBar_->setLeftFraction(leftFrac);
    }
    else
    {
        this->outcomeTitle1_->hide();
        this->outcomeDash1_->hide();
        this->outcomePct1_->hide();
        this->outcomeWon1_->hide();
        this->outcomePct0_->setText(QStringLiteral("100%"));
        this->poolBar_->setLeftFraction(1.0);
    }

    this->lastTitleForElide_ = collapsedTitled;
    {
        const QFontMetrics fm(this->collapsedTitle_->font());
        const auto elided =
            fm.elidedText(this->lastTitleForElide_, Qt::ElideRight,
                          std::max(80, this->width() - 160));
        this->collapsedTitle_->setText(
            elided.isEmpty() ? this->lastTitleForElide_ : elided);
    }
}

void SplitPredictionPanel::hidePanel()
{
    this->pointsPollTimer_.stop();
    this->pointsInFlight_ = false;
    if (this->yourPointsValue_ != nullptr)
    {
        this->yourPointsValue_->setText(QStringLiteral("\u2014"));
    }

    this->lingerTimer_.stop();
    this->lingering_ = false;
    this->lingerEventId_.clear();
    this->currentDisplayId_.clear();
    this->lastLivePrediction_.reset();
    this->countdownTimer_.stop();
    this->predictionBettingEnds_ = QDateTime();
    this->lastTitleForElide_.clear();
    if (this->outcomeWon0_ != nullptr)
    {
        this->outcomeWon0_->hide();
    }
    if (this->outcomeWon1_ != nullptr)
    {
        this->outcomeWon1_->hide();
    }
    this->hide();
}

void SplitPredictionPanel::tickPredictionCountdown()
{
    if (!this->predictionBettingEnds_.isValid())
    {
        this->countdownTimer_.stop();
        return;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    const qint64 secs = now.secsTo(this->predictionBettingEnds_);
    if (secs <= 0)
    {
        this->statusLabel_->setText(
            QStringLiteral("Active - betting window ended"));
        this->countdownTimer_.stop();
        this->predictionBettingEnds_ = QDateTime();
        return;
    }

    this->statusLabel_->setText(
        QStringLiteral("Active - closes in %1").arg(formatCountdown(secs)));
}

void SplitPredictionPanel::updateExpandToggle()
{
    this->expandedWidget_->setVisible(this->expanded_);
    this->expandButton_->setText(this->expanded_ ? QStringLiteral("▼")
                                                 : QStringLiteral("▶"));
    this->expandButton_->setToolTip(this->expanded_ ? QStringLiteral("Collapse")
                                                    : QStringLiteral("Expand"));
}

void SplitPredictionPanel::updateStyleSheets()
{
    if (this->theme == nullptr)
    {
        return;
    }

    // Match unfocused split header text: avoid focusedText (often accent/blue) on labels.
    const auto &text = this->theme->splits.header.text;
    const auto textCss = text.name(QColor::HexArgb);

    const QString labelStyle =
        QStringLiteral("QLabel { color: %1; }").arg(textCss);
    for (auto *lab :
         {this->collapsedTitle_, this->fullTitle_, this->statusLabel_,
          this->outcomeTitle0_, this->outcomeDash0_, this->outcomePct0_,
          this->outcomeTitle1_, this->outcomeDash1_, this->outcomePct1_,
          this->yourPointsLabel_, this->yourPointsValue_})
    {
        if (lab != nullptr)
        {
            lab->setStyleSheet(labelStyle);
        }
    }

    const auto accentCss = this->theme->accent.name(QColor::HexArgb);
    const QString wonStyle =
        QStringLiteral(
            "QLabel { color: %1; font-weight: 600; padding: 0 2px; }")
            .arg(accentCss);
    if (this->outcomeWon0_ != nullptr)
    {
        this->outcomeWon0_->setStyleSheet(wonStyle);
    }
    if (this->outcomeWon1_ != nullptr)
    {
        this->outcomeWon1_->setStyleSheet(wonStyle);
    }

    if (this->disclaimerLabel_ != nullptr)
    {
        const auto disclaimerCss =
            this->theme->messages.textColors.system.name(QColor::HexArgb);
        this->disclaimerLabel_->setStyleSheet(
            QStringLiteral("QLabel { color: %1; }").arg(disclaimerCss));
    }

    const QString btnStyle =
        QStringLiteral("QPushButton { color: %1; text-decoration: underline; "
                       "border: none; "
                       "padding: 2px 6px; } "
                       "QPushButton:hover { color: %2; }")
            .arg(textCss, textCss);

    this->expandButton_->setStyleSheet(btnStyle);
    this->openTwitchButton_->setStyleSheet(btnStyle);
}

void SplitPredictionPanel::openPopoutChat()
{
    auto *tc = this->twitchChannel_;
    if (tc == nullptr)
    {
        return;
    }

    const auto login = tc->getName().toLower();
    if (login.isEmpty() || login.startsWith('/'))
    {
        return;
    }

    QDesktopServices::openUrl(
        QUrl(QStringLiteral("https://www.twitch.tv/popout/%1/chat?popout=")
                 .arg(login)));
}

void SplitPredictionPanel::themeChangedEvent()
{
    BaseWidget::themeChangedEvent();
    this->updateStyleSheets();
}

void SplitPredictionPanel::scaleChangedEvent(float scale)
{
    BaseWidget::scaleChangedEvent(scale);

    const int pad = static_cast<int>(4 * scale);
    const int fs = static_cast<int>(9 * std::max(1.0f, scale));
    QFont f = this->font();
    f.setPointSize(std::max(8, fs));
    for (auto *w :
         {this->collapsedTitle_, this->fullTitle_, this->statusLabel_,
          this->outcomeTitle0_, this->outcomeWon0_, this->outcomeDash0_,
          this->outcomePct0_, this->outcomeTitle1_, this->outcomeWon1_,
          this->outcomeDash1_, this->outcomePct1_, this->yourPointsLabel_,
          this->yourPointsValue_})
    {
        if (w != nullptr)
        {
            w->setFont(f);
        }
    }
    if (this->disclaimerLabel_ != nullptr)
    {
        QFont df = f;
        df.setPointSize(std::max(7, f.pointSize() - 1));
        this->disclaimerLabel_->setFont(df);
    }
    QFont bf = f;
    bf.setBold(true);
    this->collapsedTitle_->setFont(bf);

    const int barH = static_cast<int>(std::clamp(10.0f * scale, 8.0f, 16.0f));
    if (this->poolBar_ != nullptr)
    {
        this->poolBar_->setFixedHeight(barH);
    }

    if (this->expandedLayout_ != nullptr)
    {
        this->expandedLayout_->setContentsMargins(8, pad, 8, pad + 2);
    }
}

void SplitPredictionPanel::paintEvent(QPaintEvent * /*event*/)
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

void SplitPredictionPanel::resizeEvent(QResizeEvent *event)
{
    BaseWidget::resizeEvent(event);
    if (this->lastTitleForElide_.isEmpty())
    {
        return;
    }
    const QFontMetrics fm(this->collapsedTitle_->font());
    const auto elided = fm.elidedText(this->lastTitleForElide_, Qt::ElideRight,
                                      std::max(80, this->width() - 160));
    this->collapsedTitle_->setText(elided.isEmpty() ? this->lastTitleForElide_
                                                    : elided);
}

}  // namespace chatterino

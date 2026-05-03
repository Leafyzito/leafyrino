// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/SplitPredictionPanel.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "widgets/splits/PredictionPoolBar.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitCommon.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QColor>
#include <QDateTime>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <initializer_list>

namespace {

using namespace chatterino;

constexpr int POLL_INTERVAL_MS = 5'000;
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

QColor twitchOutcomeAccentColor(const QString &gqlColor, const QColor &fallback)
{
    const QString u = gqlColor.trimmed().toUpper();
    if (u == QStringLiteral("BLUE"))
    {
        return QColor(0x1e, 0x90, 0xff);
    }
    if (u == QStringLiteral("PINK"))
    {
        return QColor(0xf4, 0x5f, 0x9e);
    }
    if (u == QStringLiteral("GREEN"))
    {
        return QColor(0x00, 0xc8, 0x53);
    }
    if (u == QStringLiteral("ORANGE"))
    {
        return QColor(0xff, 0x8c, 0x00);
    }
    if (u == QStringLiteral("PURPLE"))
    {
        return QColor(0x91, 0x46, 0xff);
    }
    if (u == QStringLiteral("GREY") || u == QStringLiteral("GRAY"))
    {
        return QColor(0xad, 0xb5, 0xbd);
    }
    return fallback;
}

bool predictionOutcomeIdsEqual(const QString &a, const QString &b)
{
    const QString xa = a.trimmed();
    const QString xb = b.trimmed();
    if (xa.isEmpty() || xb.isEmpty())
    {
        return false;
    }
    if (xa.compare(xb, Qt::CaseInsensitive) == 0)
    {
        return true;
    }
    return xa.endsWith(xb, Qt::CaseInsensitive) ||
           xb.endsWith(xa, Qt::CaseInsensitive);
}

bool viewerPickMatchesOutcomes(const HelixPrediction &p, const QString &pickRaw)
{
    const QString pick = pickRaw.trimmed();
    if (pick.isEmpty() || p.outcomes.size() != 2)
    {
        return false;
    }
    return predictionOutcomeIdsEqual(pick, p.outcomes[0].id) ||
           predictionOutcomeIdsEqual(pick, p.outcomes[1].id);
}

QUrl predictionStartSoundUrl()
{
    if (!getSettings()->predictionStartCustomSound)
    {
        return QUrl(QStringLiteral("qrc:/sounds/ping2.wav"));
    }

    const QString path = getSettings()->predictionStartSoundPath.getValue();
    if (path.trimmed().isEmpty())
    {
        return QUrl(QStringLiteral("qrc:/sounds/ping2.wav"));
    }

    return QUrl::fromLocalFile(path);
}

QString formatPointsCompact(int points)
{
    points = std::max(0, points);
    if (points < 1000)
    {
        return QString::number(points);
    }

    auto fmt = [](double v, const QChar suffix, int decimals) {
        return QStringLiteral("%1%2").arg(QString::number(v, 'f', decimals),
                                          suffix);
    };

    if (points < 1'000'000)
    {
        const double k = static_cast<double>(points) / 1000.0;
        const bool whole = std::fabs(k - std::round(k)) < 0.0001;
        return fmt(k, QLatin1Char('K'), whole ? 0 : 1);
    }

    const double m = static_cast<double>(points) / 1'000'000.0;
    const bool whole = std::fabs(m - std::round(m)) < 0.0001;
    const int decimals = whole ? 0 : (m >= 10.0 ? 1 : 2);
    return fmt(m, QLatin1Char('M'), decimals);
}

QString formatReturnRatio(double ratio)
{
    if (!(ratio > 0.0) || !std::isfinite(ratio))
    {
        return QStringLiteral("—");
    }
    return QStringLiteral("%1x").arg(QString::number(ratio, 'f', 2));
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
    this->dismissButton_->setToolTip(
        QStringLiteral("Dismiss for this prediction"));
    QObject::connect(this->dismissButton_, &QPushButton::clicked, this, [this] {
        if (this->currentDisplayId_.isEmpty())
        {
            return;
        }
        this->dismissedForPredictionId_ = this->currentDisplayId_;
        this->resetPredictionUiState();
    });

    this->expandButton_ = new QPushButton(topRow);
    this->expandButton_->setFlat(true);
    this->expandButton_->setCursor(Qt::PointingHandCursor);
    this->expandButton_->setFocusPolicy(Qt::NoFocus);
    QObject::connect(this->expandButton_, &QPushButton::clicked, this, [this] {
        this->expanded_ = !this->expanded_;
        this->updateExpandToggle();
    });

    topLayout->addWidget(this->iconLabel_, 0, Qt::AlignVCenter);
    topLayout->addWidget(this->collapsedTitle_, 1);
    topLayout->addWidget(this->expandButton_, 0, Qt::AlignRight);
    topLayout->addWidget(this->dismissButton_, 0, Qt::AlignRight);

    this->expandedWidget_ = new QWidget(this);
    this->expandedLayout_ = new QVBoxLayout(this->expandedWidget_);
    this->expandedLayout_->setContentsMargins(8, 0, 8, 6);
    this->expandedLayout_->setSpacing(4);

    this->predictionQuestionLabel_ = new QLabel(this->expandedWidget_);
    this->predictionQuestionLabel_->setWordWrap(true);

    this->statusLabel_ = new QLabel(this->expandedWidget_);

    this->yourPickSummaryLabel_ = new QLabel(this->expandedWidget_);
    this->yourPickSummaryLabel_->setWordWrap(true);
    this->yourPickSummaryLabel_->hide();

    this->outcomeRow_ = new QWidget(this->expandedWidget_);
    {
        auto *hl = new QHBoxLayout(this->outcomeRow_);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(4);

        auto *leftWrap = new QWidget(this->outcomeRow_);
        auto *leftLay = new QVBoxLayout(leftWrap);
        leftLay->setContentsMargins(0, 0, 0, 0);
        leftLay->setSpacing(0);
        this->outcomeTitle0_ = new QLabel(leftWrap);
        this->outcomeTitle0_->setWordWrap(false);
        this->outcomeDetails0_ = new QLabel(leftWrap);
        this->outcomeDetails0_->setWordWrap(false);
        this->outcomeWon0_ = new QLabel(QStringLiteral("Won"), leftWrap);
        auto *leftTop = new QWidget(leftWrap);
        auto *leftTopLay = new QHBoxLayout(leftTop);
        leftTopLay->setContentsMargins(0, 0, 0, 0);
        leftTopLay->setSpacing(4);
        leftTopLay->addWidget(this->outcomeTitle0_, 0);
        leftTopLay->addWidget(this->outcomeWon0_, 0);
        leftTopLay->addStretch(1);
        leftLay->addWidget(leftTop);
        leftLay->addWidget(this->outcomeDetails0_);

        auto *rightWrap = new QWidget(this->outcomeRow_);
        auto *rightLay = new QVBoxLayout(rightWrap);
        rightLay->setContentsMargins(0, 0, 0, 0);
        rightLay->setSpacing(0);
        this->outcomeWon1_ = new QLabel(QStringLiteral("Won"), rightWrap);
        this->outcomeTitle1_ = new QLabel(rightWrap);
        this->outcomeTitle1_->setWordWrap(false);
        this->outcomeDetails1_ = new QLabel(rightWrap);
        this->outcomeDetails1_->setWordWrap(false);
        this->outcomeDetails1_->setAlignment(Qt::AlignRight);

        auto *rightTop = new QWidget(rightWrap);
        auto *rightTopLay = new QHBoxLayout(rightTop);
        rightTopLay->setContentsMargins(0, 0, 0, 0);
        rightTopLay->setSpacing(4);
        rightTopLay->addStretch(1);
        rightTopLay->addWidget(this->outcomeWon1_, 0);
        rightTopLay->addWidget(this->outcomeTitle1_, 0);
        rightLay->addWidget(rightTop);
        rightLay->addWidget(this->outcomeDetails1_);

        this->outcomeWon0_->hide();
        this->outcomeWon1_->hide();

        hl->addWidget(leftWrap, 0);
        hl->addStretch(1);
        hl->addWidget(rightWrap, 0);
    }

    this->expandedLayout_->addWidget(this->predictionQuestionLabel_);
    this->expandedLayout_->addWidget(this->statusLabel_);
    this->expandedLayout_->addWidget(this->yourPickSummaryLabel_);
    this->expandedLayout_->addWidget(this->outcomeRow_);

    this->poolBar_ = new PredictionPoolBar(this->expandedWidget_);
    this->expandedLayout_->addWidget(this->poolBar_);

    auto *pointsRow = new QWidget(this->expandedWidget_);
    auto *pointsLay = new QHBoxLayout(pointsRow);
    pointsLay->setContentsMargins(0, 0, 0, 0);
    pointsLay->setSpacing(6);
    this->yourPointsLabel_ =
        new QLabel(QStringLiteral("Your points:"), pointsRow);
    this->yourPointsValue_ = new QLabel(QStringLiteral("-"), pointsRow);
    pointsLay->addWidget(this->yourPointsLabel_, 0);
    pointsLay->addWidget(this->yourPointsValue_, 0);
    pointsLay->addStretch(1);
    this->expandedLayout_->addWidget(pointsRow);

    this->betRow_ = new QWidget(this->expandedWidget_);
    {
        auto *betOuter = new QVBoxLayout(this->betRow_);
        betOuter->setContentsMargins(0, 0, 0, 0);
        betOuter->setSpacing(4);

        auto *betAmtRow = new QWidget(this->betRow_);
        auto *betAmtLay = new QHBoxLayout(betAmtRow);
        betAmtLay->setContentsMargins(0, 0, 0, 0);
        betAmtLay->setSpacing(6);
        this->betAmountCaption_ =
            new QLabel(QStringLiteral("Points to predict:"), betAmtRow);
        this->betAmountSpin_ = new QSpinBox(betAmtRow);
        this->betAmountSpin_->setRange(10, 10);
        this->betAmountSpin_->setSingleStep(10);
        this->betAmountSpin_->setFocusPolicy(Qt::ClickFocus);
        betAmtLay->addWidget(this->betAmountCaption_, 0);
        betAmtLay->addWidget(this->betAmountSpin_, 0);
        betAmtLay->addStretch(1);

        auto *betBtnRow = new QWidget(this->betRow_);
        auto *betBtnLay = new QHBoxLayout(betBtnRow);
        betBtnLay->setContentsMargins(0, 0, 0, 0);
        betBtnLay->setSpacing(6);
        this->betButton0_ = new QPushButton(betBtnRow);
        this->betButton1_ = new QPushButton(betBtnRow);
        this->betButton0_->setFlat(true);
        this->betButton1_->setFlat(true);
        this->betButton0_->setCursor(Qt::PointingHandCursor);
        this->betButton1_->setCursor(Qt::PointingHandCursor);
        this->betButton0_->setFocusPolicy(Qt::NoFocus);
        this->betButton1_->setFocusPolicy(Qt::NoFocus);
        QObject::connect(this->betButton0_, &QPushButton::clicked, this,
                         [this] {
                             this->placePredictionBet(0);
                         });
        QObject::connect(this->betButton1_, &QPushButton::clicked, this,
                         [this] {
                             this->placePredictionBet(1);
                         });
        betBtnLay->addWidget(this->betButton0_, 1);
        betBtnLay->addWidget(this->betButton1_, 1);

        betOuter->addWidget(betAmtRow);
        betOuter->addWidget(betBtnRow);
    }
    this->betRow_->hide();
    this->expandedLayout_->addWidget(this->betRow_);

    mainLayout->addWidget(topRow);

    this->collapsedPoolBar_ = new PredictionPoolBar(this);
    this->collapsedPoolBar_->setOutcomeMeta({}, {}, {}, {});
    this->collapsedPoolBar_->setRounded(false);
    this->collapsedPoolBar_->hide();
    mainLayout->addWidget(this->collapsedPoolBar_);

    mainLayout->addWidget(this->expandedWidget_);

    this->installClickFocusesSplit(this);

    getSettings()->showPredictionPanel.connect(
        [this](const bool & /*enabled*/) {
            this->startOrStopTimer();
            if (!getSettings()->showPredictionPanel ||
                this->split_->perSplitHidePrediction())
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

void SplitPredictionPanel::installClickFocusesSplit(QWidget *root)
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

bool SplitPredictionPanel::eventFilter(QObject *watched, QEvent *event)
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

void SplitPredictionPanel::setTwitchChannel(TwitchChannel *channel)
{
    this->channelConnections_.clear();
    this->twitchChannel_ = channel;
    this->pollTimer_.stop();
    this->countdownTimer_.stop();
    this->pointsPollTimer_.stop();
    this->predictionBettingEnds_ = QDateTime();
    this->inFlight_ = false;
    this->lastChannelPointsBalance_.reset();
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

void SplitPredictionPanel::refresh()
{
    this->fetchPredictions();
}

void SplitPredictionPanel::recoverDismissedPanel()
{
    this->dismissedForPredictionId_.clear();
    this->refresh();
}

void SplitPredictionPanel::startOrStopTimer()
{
    this->pollTimer_.stop();

    if (!getSettings()->showPredictionPanel ||
        this->split_->perSplitHidePrediction())
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
    if (!getSettings()->showPredictionPanel ||
        this->split_->perSplitHidePrediction())
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
                    if (this->lastLivePrediction_->outcomes.size() != 2)
                    {
                        this->hidePanel();
                        return;
                    }
                    if (!this->dismissedForPredictionId_.isEmpty() &&
                        this->lastLivePrediction_->id ==
                            this->dismissedForPredictionId_)
                    {
                        return;
                    }
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

            if (!this->dismissedForPredictionId_.isEmpty() &&
                pr.id == this->dismissedForPredictionId_)
            {
                return;
            }

            if (!this->currentDisplayId_.isEmpty() &&
                pr.id != this->currentDisplayId_)
            {
                this->lingerTimer_.stop();
                this->lingering_ = false;
                this->lingerEventId_.clear();
                this->clearViewerPickCache();
            }

            if (isResolvedLike(pr))
            {
                if (pr.outcomes.size() != 2)
                {
                    this->hidePanel();
                    return;
                }
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
            HelixPrediction merged = pr;
            if (merged.outcomes.size() != 2)
            {
                this->hidePanel();
                return;
            }
            if (this->lastLivePrediction_.has_value())
            {
                const auto &prev = *this->lastLivePrediction_;
                if (prev.id == merged.id &&
                    !prev.viewerPredictionOutcomeId.trimmed().isEmpty() &&
                    !viewerPickMatchesOutcomes(
                        merged, merged.viewerPredictionOutcomeId))
                {
                    merged.viewerPredictionOutcomeId =
                        prev.viewerPredictionOutcomeId;
                    merged.viewerPredictionPoints = prev.viewerPredictionPoints;
                }
            }
            this->lastLivePrediction_ = merged;
            if (viewerPickMatchesOutcomes(merged,
                                          merged.viewerPredictionOutcomeId))
            {
                this->rememberViewerPickForEvent(
                    merged.id, merged.viewerPredictionOutcomeId.trimmed(),
                    merged.viewerPredictionPoints);
            }

            const bool isNewId = this->currentDisplayId_.isEmpty() ||
                                 pr.id != this->currentDisplayId_;
            if (isNewId && getSettings()->predictionStartPlaySound)
            {
                getApp()->getSound()->play(predictionStartSoundUrl());
            }

            this->currentDisplayId_ = pr.id;
            this->renderPrediction(merged, true);
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
    if (!getSettings()->showPredictionPanel ||
        this->split_->perSplitHidePrediction())
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
        fetchLogin, acc->getOAuthToken(), acc->getOAuthClient(), this,
        [this, fetchLogin](int balance) {
            this->pointsInFlight_ = false;
            if (!this->isVisible() || this->twitchChannel_ == nullptr ||
                this->twitchChannel_->getName().trimmed().compare(
                    fetchLogin, Qt::CaseInsensitive) != 0)
            {
                return;
            }
            this->lastChannelPointsBalance_ = balance;
            if (this->yourPointsValue_ != nullptr)
            {
                this->yourPointsValue_->setText(QString::number(balance));
            }
            this->syncPredictionBetRow();
        },
        [this, fetchLogin](const QString &) {
            this->pointsInFlight_ = false;
            if (!this->isVisible() || this->twitchChannel_ == nullptr ||
                this->twitchChannel_->getName().trimmed().compare(
                    fetchLogin, Qt::CaseInsensitive) != 0)
            {
                return;
            }
            this->lastChannelPointsBalance_.reset();
            if (this->yourPointsValue_ != nullptr)
            {
                this->yourPointsValue_->setText(QStringLiteral("-"));
            }
            this->syncPredictionBetRow();
        });
}

void SplitPredictionPanel::renderPrediction(const HelixPrediction &prediction,
                                            bool liveMode)
{
    this->predictionUiLiveMode_ = liveMode;

    if (!getSettings()->showPredictionPanel ||
        this->split_->perSplitHidePrediction())
    {
        this->hidePanel();
        return;
    }

    const auto &outcomes = prediction.outcomes;
    if (outcomes.size() != 2)
    {
        this->hidePanel();
        return;
    }

    if (this->predictionQuestionLabel_ != nullptr)
    {
        this->predictionQuestionLabel_->setText(prediction.title);
    }

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
                            QStringLiteral("Active - predictions locked");
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
    const HelixPredictionOutcome *o1 = &outcomes[1];

    int p0 = std::max(0, o0->channelPoints);
    int p1 = std::max(0, o1->channelPoints);
    const int total = p0 + p1;
    const bool canShowCollapsedBar = (total > 0);
    int pct0 = 50;
    int pct1 = 50;
    if (total > 0)
    {
        pct0 = static_cast<int>((100LL * p0 + total / 2) / total);
        pct1 = 100 - pct0;
    }

    this->outcomeTitle0_->setText(o0->title);
    const double r0 =
        (total > 0 && p0 > 0) ? static_cast<double>(total) / p0 : 0.0;
    const QString pct0s = QStringLiteral("%1%").arg(pct0);
    this->outcomeDetails0_->setText(
        QStringLiteral("%1 • %2 • %3")
            .arg(formatPointsCompact(p0), formatReturnRatio(r0), pct0s));

    this->outcomeWon0_->setVisible(wSide == 0);
    this->outcomeWon1_->setVisible(wSide == 1);

    this->outcomeTitle1_->show();
    this->outcomeDetails1_->show();
    this->outcomeTitle1_->setText(o1->title);
    const double leftFrac =
        total > 0 ? static_cast<double>(p0) / static_cast<double>(total) : 0.5;
    this->poolBar_->setLeftFraction(leftFrac);
    if (this->collapsedPoolBar_ != nullptr)
    {
        if (canShowCollapsedBar)
        {
            this->collapsedPoolBar_->setLeftFraction(leftFrac);
            this->collapsedPoolBar_->setVisible(!this->expanded_);
        }
        else
        {
            this->collapsedPoolBar_->hide();
        }
    }

    const double r1 =
        (total > 0 && p1 > 0) ? static_cast<double>(total) / p1 : 0.0;
    const QString pct1s = QStringLiteral("%1%").arg(pct1);
    this->outcomeDetails1_->setText(
        QStringLiteral("%1 • %2 • %3")
            .arg(pct1s, formatReturnRatio(r1), formatPointsCompact(p1)));
    this->poolBar_->setOutcomeMeta({}, {}, {}, {});

    this->lastTitleForElide_ = prediction.title;
    this->refreshTopRowText();

    this->syncPredictionBetRow();
}

void SplitPredictionPanel::resetPredictionUiState()
{
    this->pointsPollTimer_.stop();
    this->pointsInFlight_ = false;
    this->betInFlight_ = false;
    this->lastChannelPointsBalance_.reset();
    if (this->yourPointsValue_ != nullptr)
    {
        this->yourPointsValue_->setText(QStringLiteral("-"));
    }
    if (this->betRow_ != nullptr)
    {
        this->betRow_->hide();
    }

    this->lingerTimer_.stop();
    this->lingering_ = false;
    this->lingerEventId_.clear();
    this->currentDisplayId_.clear();
    this->lastLivePrediction_.reset();
    this->countdownTimer_.stop();
    this->predictionBettingEnds_ = QDateTime();
    this->lastTitleForElide_.clear();
    if (this->predictionQuestionLabel_ != nullptr)
    {
        this->predictionQuestionLabel_->clear();
    }
    if (this->outcomeWon0_ != nullptr)
    {
        this->outcomeWon0_->hide();
    }
    if (this->outcomeWon1_ != nullptr)
    {
        this->outcomeWon1_->hide();
    }
    if (this->outcomeDetails0_ != nullptr)
    {
        this->outcomeDetails0_->clear();
    }
    if (this->outcomeDetails1_ != nullptr)
    {
        this->outcomeDetails1_->clear();
    }
    if (this->yourPickSummaryLabel_ != nullptr)
    {
        this->yourPickSummaryLabel_->hide();
        this->yourPickSummaryLabel_->clear();
    }
    this->predictionUiLiveMode_ = false;
    this->clearViewerPickCache();
    if (this->collapsedPoolBar_ != nullptr)
    {
        this->collapsedPoolBar_->hide();
        this->collapsedPoolBar_->setOutcomeMeta({}, {}, {}, {});
        this->collapsedPoolBar_->setLeftFraction(0.5);
    }
    this->hide();
}

void SplitPredictionPanel::hidePanel()
{
    this->dismissedForPredictionId_.clear();
    this->resetPredictionUiState();
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
            QStringLiteral("Active - predictions locked"));
        this->countdownTimer_.stop();
        this->predictionBettingEnds_ = QDateTime();
        this->syncPredictionBetRow();
        return;
    }

    this->statusLabel_->setText(
        QStringLiteral("Active - closes in %1").arg(formatCountdown(secs)));
    this->syncPredictionBetRow();
}

void SplitPredictionPanel::refreshTopRowText()
{
    if (this->collapsedTitle_ == nullptr || this->lastTitleForElide_.isEmpty())
    {
        return;
    }

    if (this->expanded_)
    {
        this->collapsedTitle_->setText(QStringLiteral("Prediction"));
        return;
    }

    this->updateCollapsedElide();
}

void SplitPredictionPanel::updateCollapsedElide()
{
    if (this->collapsedTitle_ == nullptr ||
        this->lastTitleForElide_.isEmpty() || this->expanded_)
    {
        return;
    }

    // Mirror `SplitPinnedMessagePanel` collapsed width behavior:
    // derive available width from the actual top-row layout (not magic numbers),
    // and avoid early-layout width=0 producing overly aggressive elides.
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

    const QFontMetrics fm(this->collapsedTitle_->font());
    const QString elided =
        fm.elidedText(this->lastTitleForElide_, Qt::ElideRight, avail);
    this->collapsedTitle_->setText(elided.isEmpty() ? this->lastTitleForElide_
                                                    : elided);
}

void SplitPredictionPanel::updateExpandToggle()
{
    this->expandedWidget_->setVisible(this->expanded_);
    if (this->collapsedPoolBar_ != nullptr)
    {
        // Only show when collapsed and we have a meaningful 2-outcome pool.
        // Visibility is also updated in renderPrediction based on data availability.
        if (this->expanded_)
        {
            this->collapsedPoolBar_->hide();
        }
        else if (this->lastLivePrediction_.has_value() &&
                 this->lastLivePrediction_->outcomes.size() == 2)
        {
            const int p0 = std::max(
                0, this->lastLivePrediction_->outcomes[0].channelPoints);
            const int p1 = std::max(
                0, this->lastLivePrediction_->outcomes[1].channelPoints);
            const int total = p0 + p1;
            if (total > 0)
            {
                this->collapsedPoolBar_->setLeftFraction(
                    static_cast<double>(p0) / static_cast<double>(total));
                this->collapsedPoolBar_->show();
            }
            else
            {
                this->collapsedPoolBar_->hide();
            }
        }
        else
        {
            this->collapsedPoolBar_->hide();
        }
    }
    this->expandButton_->setText(this->expanded_ ? QStringLiteral("▼")
                                                 : QStringLiteral("▶"));
    this->expandButton_->setToolTip(this->expanded_ ? QStringLiteral("Collapse")
                                                    : QStringLiteral("Expand"));
    this->refreshTopRowText();
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
         {this->collapsedTitle_, this->predictionQuestionLabel_,
          this->statusLabel_, this->yourPickSummaryLabel_, this->outcomeTitle0_,
          this->outcomeDetails0_, this->outcomeTitle1_, this->outcomeDetails1_,
          this->yourPointsLabel_, this->yourPointsValue_,
          this->betAmountCaption_})
    {
        if (lab != nullptr)
        {
            lab->setStyleSheet(labelStyle);
        }
    }

    if (this->poolBar_ != nullptr)
    {
        // Used by PredictionPoolBar::paintEvent for overlay text
        this->poolBar_->setStyleSheet(
            QStringLiteral("PredictionPoolBar { color: %1; }").arg(textCss));
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
    this->expandButton_->setStyleSheet(btnStyle);

    if (this->betAmountSpin_ != nullptr)
    {
        this->betAmountSpin_->setStyleSheet(
            QStringLiteral("QSpinBox { color: %1; padding: 1px 4px; }")
                .arg(textCss));
    }

    this->refreshBetOutcomeButtonStyles();
}

void SplitPredictionPanel::updatePredictionIcon()
{
    if (this->iconLabel_ == nullptr || this->theme == nullptr)
    {
        return;
    }

    const int px = splitHeaderIconColumnWidth(this->scale());
    const QString iconPath =
        this->theme->isLightTheme()
            ? QStringLiteral(":/buttons/prediction-lightMode.svg")
            : QStringLiteral(":/buttons/prediction-darkMode.svg");
    const QPixmap pm = QIcon(iconPath).pixmap(px, px);
    if (!pm.isNull())
    {
        this->iconLabel_->setPixmap(pm);
    }
    this->iconLabel_->setFixedWidth(px);
    this->iconLabel_->setToolTip(QStringLiteral("Channel points prediction"));
}

void SplitPredictionPanel::themeChangedEvent()
{
    BaseWidget::themeChangedEvent();
    this->updateStyleSheets();
    this->updatePredictionIcon();
}

void SplitPredictionPanel::scaleChangedEvent(float scale)
{
    BaseWidget::scaleChangedEvent(scale);

    const int pad = static_cast<int>(4 * scale);
    const int fs = static_cast<int>(9 * std::max(1.0f, scale));
    QFont f = this->font();
    f.setPointSize(std::max(8, fs));
    for (QWidget *w : std::initializer_list<QWidget *>{
             this->collapsedTitle_, this->predictionQuestionLabel_,
             this->statusLabel_, this->yourPickSummaryLabel_,
             this->outcomeTitle0_, this->outcomeWon0_, this->outcomeDash0_,
             this->outcomeDetails0_, this->outcomeTitle1_, this->outcomeWon1_,
             this->outcomeDetails1_, this->yourPointsLabel_,
             this->yourPointsValue_, this->betAmountCaption_,
             this->betAmountSpin_, this->betButton0_, this->betButton1_})
    {
        if (w != nullptr)
        {
            w->setFont(f);
        }
    }
    QFont bf = f;
    bf.setBold(true);
    this->collapsedTitle_->setFont(bf);

    const int barH = static_cast<int>(std::clamp(10.0f * scale, 8.0f, 16.0f));
    if (this->poolBar_ != nullptr)
    {
        this->poolBar_->setFont(f);
        this->poolBar_->setFixedHeight(barH);
    }
    if (this->collapsedPoolBar_ != nullptr)
    {
        const int collapsedH =
            static_cast<int>(std::clamp(2.0f * scale, 2.0f, 3.0f));
        this->collapsedPoolBar_->setFixedHeight(collapsedH);
    }

    if (this->expandedLayout_ != nullptr)
    {
        this->expandedLayout_->setContentsMargins(8, pad, 8, pad + 2);
    }

    this->updatePredictionIcon();
    this->refreshTopRowText();
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
    this->refreshTopRowText();
}

void SplitPredictionPanel::showEvent(QShowEvent *event)
{
    BaseWidget::showEvent(event);
    QTimer::singleShot(0, this, [this] {
        this->updateCollapsedElide();
    });
}

void SplitPredictionPanel::clearViewerPickCache()
{
    this->viewerPickCacheEventId_.clear();
    this->viewerPickCacheOutcomeId_.clear();
    this->viewerPickCachePoints_ = 0;
}

void SplitPredictionPanel::rememberViewerPickForEvent(const QString &eventId,
                                                      const QString &outcomeId,
                                                      int points)
{
    const QString o = outcomeId.trimmed();
    if (eventId.isEmpty() || o.isEmpty())
    {
        return;
    }
    this->viewerPickCacheEventId_ = eventId;
    this->viewerPickCacheOutcomeId_ = o;
    this->viewerPickCachePoints_ = points;
}

bool SplitPredictionPanel::baseBetContextForBetting() const
{
    if (!getSettings()->showPredictionPanel ||
        this->split_->perSplitHidePrediction() ||
        this->twitchChannel_ == nullptr ||
        getApp()->getAccounts()->twitch.getCurrent()->isAnon() ||
        this->lingering_ || !this->lastLivePrediction_.has_value())
    {
        return false;
    }
    const auto &pre = *this->lastLivePrediction_;
    return pre.status.compare(QStringLiteral("ACTIVE"), Qt::CaseInsensitive) ==
               0 &&
           this->predictionBettingEnds_.isValid() &&
           QDateTime::currentDateTimeUtc().secsTo(
               this->predictionBettingEnds_) > 0 &&
           pre.outcomes.size() == 2;
}

void SplitPredictionPanel::updateYourPickSummaryLabel()
{
    if (this->yourPickSummaryLabel_ == nullptr)
    {
        return;
    }

    const auto clearSummary = [this] {
        this->yourPickSummaryLabel_->hide();
        this->yourPickSummaryLabel_->clear();
    };

    if (!getSettings()->showPredictionPanel ||
        this->split_->perSplitHidePrediction() ||
        this->twitchChannel_ == nullptr ||
        getApp()->getAccounts()->twitch.getCurrent()->isAnon() ||
        this->lingering_ || !this->predictionUiLiveMode_)
    {
        clearSummary();
        return;
    }

    if (!this->lastLivePrediction_.has_value())
    {
        clearSummary();
        return;
    }

    const auto &pr = *this->lastLivePrediction_;
    if (pr.outcomes.size() != 2)
    {
        clearSummary();
        return;
    }

    QString pickId = pr.viewerPredictionOutcomeId.trimmed();
    int pickPts = pr.viewerPredictionPoints;
    if (pickId.isEmpty() || !viewerPickMatchesOutcomes(pr, pickId))
    {
        if (pr.id == this->viewerPickCacheEventId_)
        {
            pickId = this->viewerPickCacheOutcomeId_.trimmed();
            pickPts = this->viewerPickCachePoints_;
        }
    }
    if (pickId.isEmpty())
    {
        clearSummary();
        return;
    }

    const bool baseBetContext = this->baseBetContextForBetting();

    if (baseBetContext || this->betInFlight_)
    {
        clearSummary();
        return;
    }

    QString title;
    for (const auto &o : pr.outcomes)
    {
        if (predictionOutcomeIdsEqual(pickId, o.id))
        {
            title = o.title;
            break;
        }
    }
    if (title.isEmpty())
    {
        clearSummary();
        return;
    }

    this->yourPickSummaryLabel_->setText(
        QStringLiteral("Your pick - %1 (%2 pts)").arg(title).arg(pickPts));
    this->yourPickSummaryLabel_->show();
}

void SplitPredictionPanel::syncPredictionBetRow()
{
    if (this->betRow_ == nullptr)
    {
        this->updateYourPickSummaryLabel();
        return;
    }

    constexpr int kBetMin = 10;
    constexpr int kBetMaxSite = 250'000;

    if (!this->lastLivePrediction_.has_value())
    {
        if (!this->betInFlight_)
        {
            this->betRow_->hide();
        }
        this->updateYourPickSummaryLabel();
        return;
    }

    const bool baseBetContext = this->baseBetContextForBetting();

    if (!baseBetContext && !this->betInFlight_)
    {
        this->betRow_->hide();
        this->updateYourPickSummaryLabel();
        return;
    }

    const auto &pr = *this->lastLivePrediction_;
    const QString pickId = pr.viewerPredictionOutcomeId.trimmed();
    const bool hasPick = !pickId.isEmpty();
    const bool picked0 =
        hasPick && predictionOutcomeIdsEqual(pickId, pr.outcomes[0].id);
    const bool picked1 =
        hasPick && predictionOutcomeIdsEqual(pickId, pr.outcomes[1].id);

    const bool canSpend = this->lastChannelPointsBalance_.has_value() &&
                          *this->lastChannelPointsBalance_ >= kBetMin;

    const bool showRow =
        baseBetContext && (canSpend || hasPick || this->betInFlight_);

    if (!showRow)
    {
        this->betRow_->hide();
        this->updateYourPickSummaryLabel();
        return;
    }

    this->betRow_->show();

    const bool allowInteract =
        baseBetContext && canSpend && !this->betInFlight_;

    if (canSpend)
    {
        const int bal = *this->lastChannelPointsBalance_;
        const int maxPts = std::min(kBetMaxSite, bal);
        this->betAmountSpin_->setMinimum(kBetMin);
        this->betAmountSpin_->setMaximum(maxPts);
        const int v =
            std::clamp(this->betAmountSpin_->value(), kBetMin, maxPts);
        this->betAmountSpin_->setValue(v);
    }
    this->betAmountSpin_->setEnabled(allowInteract);

    if (picked0)
    {
        this->betButton0_->setText(QStringLiteral("Your pick - %1 (%2 pts)")
                                       .arg(pr.outcomes[0].title)
                                       .arg(pr.viewerPredictionPoints));
    }
    else
    {
        this->betButton0_->setText(pr.outcomes[0].title);
    }
    if (picked1)
    {
        this->betButton1_->setText(QStringLiteral("Your pick - %1 (%2 pts)")
                                       .arg(pr.outcomes[1].title)
                                       .arg(pr.viewerPredictionPoints));
    }
    else
    {
        this->betButton1_->setText(pr.outcomes[1].title);
    }

    if (picked0)
    {
        this->betButton1_->setToolTip(
            QStringLiteral(
                "You can only choose one outcome. You already predicted “%1”.")
                .arg(pr.outcomes[0].title));
        this->betButton0_->setToolTip({});
    }
    else if (picked1)
    {
        this->betButton0_->setToolTip(
            QStringLiteral(
                "You can only choose one outcome. You already predicted “%1”.")
                .arg(pr.outcomes[1].title));
        this->betButton1_->setToolTip({});
    }
    else
    {
        this->betButton0_->setToolTip({});
        this->betButton1_->setToolTip({});
    }

    const bool canBet0 = allowInteract && !picked1;
    const bool canBet1 = allowInteract && !picked0;
    this->betButton0_->setEnabled(canBet0);
    this->betButton1_->setEnabled(canBet1);

    this->refreshBetOutcomeButtonStyles();
    this->updateYourPickSummaryLabel();
}

void SplitPredictionPanel::refreshBetOutcomeButtonStyles()
{
    if (this->theme == nullptr || this->betButton0_ == nullptr ||
        this->betButton1_ == nullptr)
    {
        return;
    }
    if (!this->lastLivePrediction_.has_value() ||
        this->lastLivePrediction_->outcomes.size() < 2)
    {
        return;
    }
    const auto &pr = *this->lastLivePrediction_;
    const QColor labelColor = this->theme->splits.header.text;
    const QColor accent = this->theme->accent;

    const QColor c0 = twitchOutcomeAccentColor(pr.outcomes[0].color, accent);
    const QColor c1 = twitchOutcomeAccentColor(pr.outcomes[1].color, accent);

    this->betButton0_->setStyleSheet(splitAccentActionButtonStyleSheet(
        c0, labelColor, this->betButton0_->isEnabled()));
    this->betButton1_->setStyleSheet(splitAccentActionButtonStyleSheet(
        c1, labelColor, this->betButton1_->isEnabled()));
}

void SplitPredictionPanel::placePredictionBet(int outcomeIndex)
{
    if (this->betInFlight_ || !this->lastLivePrediction_.has_value())
    {
        return;
    }

    const auto &pr = *this->lastLivePrediction_;
    if (outcomeIndex < 0 ||
        static_cast<size_t>(outcomeIndex) >= pr.outcomes.size())
    {
        return;
    }

    const QString existingPick = pr.viewerPredictionOutcomeId.trimmed();
    if (!existingPick.isEmpty())
    {
        const QString chosenId =
            pr.outcomes[static_cast<size_t>(outcomeIndex)].id;
        if (!predictionOutcomeIdsEqual(existingPick, chosenId))
        {
            return;
        }
    }

    const auto acc = getApp()->getAccounts()->twitch.getCurrent();
    if (acc->isAnon())
    {
        return;
    }

    const int points = this->betAmountSpin_->value();
    if (points < 10)
    {
        return;
    }

    const QString eventId = pr.id;
    const QString outcomeId = pr.outcomes[static_cast<size_t>(outcomeIndex)].id;
    if (eventId.isEmpty() || outcomeId.isEmpty())
    {
        return;
    }

    this->betInFlight_ = true;
    this->syncPredictionBetRow();
    this->statusLabel_->setText(QStringLiteral("Placing prediction…"));

    const QString fetchRoomId =
        this->twitchChannel_ ? this->twitchChannel_->roomId() : QString();

    TwitchGql::makePrediction(
        eventId, outcomeId, points, acc->getOAuthToken(), acc->getOAuthClient(),
        this,
        [this, fetchRoomId, eventId, outcomeId, points]() {
            this->betInFlight_ = false;
            if (this->twitchChannel_ == nullptr ||
                (!fetchRoomId.isEmpty() &&
                 this->twitchChannel_->roomId() != fetchRoomId))
            {
                return;
            }
            if (this->lastLivePrediction_.has_value() &&
                this->lastLivePrediction_->id == eventId)
            {
                auto &mutablePred = *this->lastLivePrediction_;
                const QString o = outcomeId.trimmed();
                const QString cur =
                    mutablePred.viewerPredictionOutcomeId.trimmed();
                if (cur.isEmpty() || predictionOutcomeIdsEqual(cur, o))
                {
                    mutablePred.viewerPredictionOutcomeId = outcomeId;
                    mutablePred.viewerPredictionPoints += points;
                }
                this->rememberViewerPickForEvent(
                    eventId, mutablePred.viewerPredictionOutcomeId.trimmed(),
                    mutablePred.viewerPredictionPoints);
            }
            this->statusLabel_->setText(QStringLiteral("Prediction placed"));
            this->syncPredictionBetRow();
            this->fetchPredictions();
            this->fetchChannelPoints();
        },
        [this, fetchRoomId](const QString &err) {
            this->betInFlight_ = false;
            qCWarning(chatterinoTwitch) << "MakePrediction failed:" << err;
            if (this->twitchChannel_ == nullptr ||
                (!fetchRoomId.isEmpty() &&
                 this->twitchChannel_->roomId() != fetchRoomId))
            {
                return;
            }
            this->statusLabel_->setText(
                QStringLiteral("Prediction failed: %1").arg(err));
            this->syncPredictionBetRow();
            if (this->predictionBettingEnds_.isValid() &&
                QDateTime::currentDateTimeUtc().secsTo(
                    this->predictionBettingEnds_) > 0)
            {
                this->tickPredictionCountdown();
            }
        });
}

}  // namespace chatterino

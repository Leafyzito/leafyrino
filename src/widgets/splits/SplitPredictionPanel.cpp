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
#include <QVBoxLayout>

#include <algorithm>
#include <initializer_list>

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

QString betOutcomeButtonStyleSheet(const QColor &accent,
                                   const QColor &labelColor, bool enabled)
{
    const QString accentCss = accent.name(QColor::HexArgb);
    const QString labelCss = labelColor.name(QColor::HexArgb);
    if (!enabled)
    {
        const QColor dimText =
            QColor::fromRgbF(labelColor.redF() * 0.5, labelColor.greenF() * 0.5,
                             labelColor.blueF() * 0.5, labelColor.alphaF());
        const QColor dimBorder =
            QColor::fromRgbF(accent.redF() * 0.45 + dimText.redF() * 0.25,
                             accent.greenF() * 0.45 + dimText.greenF() * 0.25,
                             accent.blueF() * 0.45 + dimText.blueF() * 0.25,
                             std::max(0.35f, accent.alphaF()));
        return QStringLiteral(
                   "QPushButton { color: %1; text-decoration: none; "
                   "border: 2px solid %2; border-radius: 4px; padding: 4px "
                   "8px; "
                   "background-color: transparent; font-style: italic; }")
            .arg(dimText.name(QColor::HexArgb),
                 dimBorder.name(QColor::HexArgb));
    }
    const int r = accent.red(), g = accent.green(), b = accent.blue();
    const QString bg1 = QColor(r, g, b, 55).name(QColor::HexArgb);
    const QString bg2 = QColor(r, g, b, 90).name(QColor::HexArgb);
    return QStringLiteral(
               "QPushButton { color: %1; text-decoration: none; "
               "border: 2px solid %2; border-radius: 4px; padding: 4px 8px; "
               "background-color: %3; font-weight: 600; }"
               "QPushButton:hover { background-color: %4; }")
        .arg(labelCss, accentCss, bg1, bg2);
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

    this->expandedLayout_->addWidget(this->predictionQuestionLabel_);
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

    this->expandedLayout_->addSpacing(6);
    this->disclaimerLabel_ = new QLabel(
        QStringLiteral(
            "Channel points features are in beta. They may change or stop "
            "working if Twitch updates their API."),
        this->expandedWidget_);
    this->disclaimerLabel_->setWordWrap(true);
    this->disclaimerLabel_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    this->expandedLayout_->addWidget(this->disclaimerLabel_);

    mainLayout->addWidget(topRow);
    mainLayout->addWidget(this->expandedWidget_);

    this->installClickFocusesSplit(this);

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
            HelixPrediction merged = pr;
            if (this->lastLivePrediction_.has_value())
            {
                const auto &prev = *this->lastLivePrediction_;
                if (prev.id == merged.id &&
                    !prev.viewerPredictionOutcomeId.trimmed().isEmpty() &&
                    merged.viewerPredictionOutcomeId.trimmed().isEmpty())
                {
                    merged.viewerPredictionOutcomeId =
                        prev.viewerPredictionOutcomeId;
                    merged.viewerPredictionPoints = prev.viewerPredictionPoints;
                }
            }
            this->lastLivePrediction_ = merged;
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
          this->statusLabel_, this->outcomeTitle0_, this->outcomeDash0_,
          this->outcomePct0_, this->outcomeTitle1_, this->outcomeDash1_,
          this->outcomePct1_, this->yourPointsLabel_, this->yourPointsValue_,
          this->betAmountCaption_})
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
             this->statusLabel_, this->outcomeTitle0_, this->outcomeWon0_,
             this->outcomeDash0_, this->outcomePct0_, this->outcomeTitle1_,
             this->outcomeWon1_, this->outcomeDash1_, this->outcomePct1_,
             this->yourPointsLabel_, this->yourPointsValue_,
             this->betAmountCaption_, this->betAmountSpin_, this->betButton0_,
             this->betButton1_})
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

void SplitPredictionPanel::syncPredictionBetRow()
{
    if (this->betRow_ == nullptr)
    {
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
        return;
    }

    bool baseBetContext = false;
    if (getSettings()->showPredictionPanel && this->twitchChannel_ != nullptr &&
        !getApp()->getAccounts()->twitch.getCurrent()->isAnon() &&
        !this->lingering_)
    {
        const auto &pre = *this->lastLivePrediction_;
        baseBetContext = pre.status.compare(QStringLiteral("ACTIVE"),
                                            Qt::CaseInsensitive) == 0 &&
                         this->predictionBettingEnds_.isValid() &&
                         QDateTime::currentDateTimeUtc().secsTo(
                             this->predictionBettingEnds_) > 0 &&
                         pre.outcomes.size() == 2;
    }

    if (!baseBetContext && !this->betInFlight_)
    {
        this->betRow_->hide();
        return;
    }

    const auto &pr = *this->lastLivePrediction_;
    const QString pickId = pr.viewerPredictionOutcomeId.trimmed();
    const bool hasPick = !pickId.isEmpty();
    const QString id0 = pr.outcomes[0].id.trimmed();
    const QString id1 = pr.outcomes[1].id.trimmed();
    const bool picked0 =
        hasPick && pickId.compare(id0, Qt::CaseInsensitive) == 0;
    const bool picked1 =
        hasPick && pickId.compare(id1, Qt::CaseInsensitive) == 0;

    const bool canSpend = this->lastChannelPointsBalance_.has_value() &&
                          *this->lastChannelPointsBalance_ >= kBetMin;

    const bool showRow =
        baseBetContext && (canSpend || hasPick || this->betInFlight_);

    if (!showRow)
    {
        this->betRow_->hide();
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

    this->betButton0_->setStyleSheet(betOutcomeButtonStyleSheet(
        c0, labelColor, this->betButton0_->isEnabled()));
    this->betButton1_->setStyleSheet(betOutcomeButtonStyleSheet(
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
            pr.outcomes[static_cast<size_t>(outcomeIndex)].id.trimmed();
        if (existingPick.compare(chosenId, Qt::CaseInsensitive) != 0)
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
                if (mutablePred.viewerPredictionOutcomeId.trimmed().compare(
                        o, Qt::CaseInsensitive) == 0 ||
                    mutablePred.viewerPredictionOutcomeId.trimmed().isEmpty())
                {
                    mutablePred.viewerPredictionOutcomeId = outcomeId;
                    mutablePred.viewerPredictionPoints += points;
                }
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

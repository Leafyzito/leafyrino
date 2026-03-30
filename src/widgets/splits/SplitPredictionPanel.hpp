// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/twitch/api/Helix.hpp"
#include "widgets/BaseWidget.hpp"

#include <boost/signals2.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QDateTime>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QString>
#include <QTimer>

#include <optional>
#include <vector>

class QLabel;
class QPushButton;
class QWidget;
class QVBoxLayout;

namespace chatterino {

class PredictionPoolBar;
class Split;
class TwitchChannel;

/**
 * Read-only strip showing an ACTIVE or LOCKED channel points prediction for the
 * split's Twitch channel (Twitch GQL, polled).
 */
class SplitPredictionPanel : public BaseWidget
{
    Q_OBJECT

public:
    explicit SplitPredictionPanel(Split *split);

    /// Binds to a Twitch channel or clears when @a channel is nullptr.
    void setTwitchChannel(TwitchChannel *channel);

    /// Immediate Helix refresh (e.g. when the split gains focus).
    void refresh();

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void startOrStopTimer();
    void fetchPredictions();
    void renderPrediction(const HelixPrediction &prediction, bool liveMode);
    void hidePanel();
    void updateExpandToggle();
    void updateStyleSheets();
    void openPopoutChat();
    void tickPredictionCountdown();
    void onPanelShown();
    void fetchChannelPoints();

    Split *const split_;
    TwitchChannel *twitchChannel_{nullptr};
    QTimer pollTimer_;
    QTimer countdownTimer_;
    QTimer lingerTimer_;
    QTimer pointsPollTimer_;
    QDateTime predictionBettingEnds_;
    pajlada::Signals::SignalHolder channelConnections_;
    pajlada::Signals::SignalHolder managedConnections_;
    std::vector<boost::signals2::scoped_connection> boostConnections_;

    bool expanded_{true};
    bool inFlight_{false};
    bool pointsInFlight_{false};
    QString lastTitleForElide_;

    QWidget *expandedWidget_{};
    QVBoxLayout *expandedLayout_{};

    QLabel *collapsedTitle_{};
    QPushButton *expandButton_{};
    QPushButton *openTwitchButton_{};

    QLabel *fullTitle_{};
    QLabel *statusLabel_{};
    QWidget *outcomeRow_{};
    QLabel *outcomeTitle0_{};
    QLabel *outcomeWon0_{};
    QLabel *outcomeDash0_{};
    QLabel *outcomePct0_{};
    QLabel *outcomeTitle1_{};
    QLabel *outcomeWon1_{};
    QLabel *outcomeDash1_{};
    QLabel *outcomePct1_{};
    PredictionPoolBar *poolBar_{};
    QLabel *yourPointsLabel_{};
    QLabel *yourPointsValue_{};
    QLabel *disclaimerLabel_{};

    std::optional<HelixPrediction> lastLivePrediction_;
    QString currentDisplayId_;
    QString lingerEventId_;
    bool lingering_{false};
};

}  // namespace chatterino

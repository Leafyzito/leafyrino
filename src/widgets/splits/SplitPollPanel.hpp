// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/twitch/api/TwitchGql.hpp"
#include "widgets/BaseWidget.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QEvent>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QString>
#include <QTimer>

#include <optional>
#include <vector>

class QLabel;
class QPushButton;
class QShowEvent;
class QVBoxLayout;
class QWidget;

namespace chatterino {

class PollChoiceRow;
class Split;
class TwitchChannel;

/// Strip showing a Twitch channel poll (GQL `viewablePoll`, polled): ACTIVE with voting,
/// terminal results, and a short linger after the poll ends or disappears from GQL.
class SplitPollPanel : public BaseWidget
{
    Q_OBJECT

public:
    explicit SplitPollPanel(Split *split);

    void setTwitchChannel(TwitchChannel *channel);
    void refresh();
    void recoverDismissedPanel();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void installClickFocusesSplit(QWidget *root);
    void startOrStopTimer();
    void fetchPoll();
    void hidePollUi();
    void hidePanel();
    void onLingerTimeout();
    void updateStyleSheets();
    void updatePollIcon();
    void updateExpandToggle();
    void refreshTopRowText();
    void updateCollapsedElide();
    void rebuildChoiceRows();
    void tickCountdownDisplay();
    void applyPollToUi(const TwitchGql::ViewablePoll &poll);
    void castVote(const QString &choiceId);

    Split *const split_;
    TwitchChannel *twitchChannel_{nullptr};

    QTimer pollTimer_;
    QTimer countdownTimer_;
    QTimer lingerTimer_;
    bool inFlight_{false};
    bool voteInFlight_{false};

    constexpr static int LINGER_MS = 30'000;
    bool lingering_{false};
    QString lingerPollId_;
    QString currentDisplayPollId_;
    std::optional<TwitchGql::ViewablePoll> lastLivePoll_;

    std::optional<TwitchGql::ViewablePoll> current_;
    QString lastTitleForElide_;
    QString dismissedForPollId_;
    qint64 displayRemainingMs_{0};
    QString voteError_;
    /// GQL returned no poll but we still show `lastLivePoll_` for linger; UI reads as ended.
    bool lingerEndedDisplay_{false};

    bool expanded_{true};
    QLabel *iconLabel_{};
    QLabel *collapsedTitle_{};
    QPushButton *dismissButton_{};
    QPushButton *expandButton_{};
    QWidget *expandedWidget_{};
    QVBoxLayout *expandedLayout_{};
    QLabel *expandedQuestionLabel_{};
    QLabel *pollStatusLabel_{};
    QLabel *countdownLabel_{};
    QWidget *choicesContainer_{};
    QVBoxLayout *choicesLayout_{};
    std::vector<PollChoiceRow *> pollRows_;

    pajlada::Signals::SignalHolder channelConnections_;
    pajlada::Signals::SignalHolder managedConnections_;
};

}  // namespace chatterino

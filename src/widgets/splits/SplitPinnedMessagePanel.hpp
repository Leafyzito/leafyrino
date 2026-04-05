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

class QLabel;
class QPushButton;
class QShowEvent;
class QWidget;

namespace chatterino {

class MessageView;
class Split;
class TwitchChannel;

/// Strip showing the current moderator-pinned message for the split's Twitch channel (Twitch GQL, polled).
class SplitPinnedMessagePanel : public BaseWidget
{
    Q_OBJECT

public:
    explicit SplitPinnedMessagePanel(Split *split);

    /// Binds to a Twitch channel or clears when @a channel is nullptr.
    void setTwitchChannel(TwitchChannel *channel);

    /// Immediate refresh (e.g. when the split gains focus).
    void refresh();

    /// Clears per-event dismiss state and refetches (split menu).
    void recoverDismissedPanel();

    MessageView *expandedMessageView() const
    {
        return this->expandedMessageView_;
    }

    void clearExpandedMessageSelection();

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
    void fetchPinned();
    void hidePinnedUi();
    void hidePanel();
    void updateStyleSheets();
    void updateText();
    void updateCollapsedElide();
    void refreshTopRowText();
    void updateExpandToggle();
    void updatePinIcon();
    void updateExpandedMessageWidth();

    Split *const split_;
    TwitchChannel *twitchChannel_{nullptr};

    QTimer pollTimer_;
    bool inFlight_{false};

    std::optional<TwitchGql::PinnedChatMessage> current_;
    QString fullPinnedText_;
    QString dismissedForPinnedMessageId_;

    bool expanded_{false};
    QLabel *iconLabel_{};
    QLabel *collapsedLabel_{};
    QPushButton *dismissButton_{};
    QPushButton *expandButton_{};
    QWidget *expandedWidget_{};
    MessageView *expandedMessageView_{};

    pajlada::Signals::SignalHolder channelConnections_;
    pajlada::Signals::SignalHolder managedConnections_;
};

}  // namespace chatterino

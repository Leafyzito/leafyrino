#pragma once

#include "common/Channel.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "widgets/BaseWidget.hpp"
#include "widgets/buttons/SvgButton.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QHBoxLayout>
#include <QLabel>

#include <vector>

namespace chatterino {

class Split;
class ChannelView;

class PinnedMessageBanner : public BaseWidget
{
    Q_OBJECT

public:
    explicit PinnedMessageBanner(Split *split, QWidget *parent = nullptr);

    void setPinnedMessage(
        const std::optional<TwitchChannel::PinnedMessage> &pin,
        TwitchChannel *channel);

    bool hasPinnedMessage() const;

    void setToggleButtonVisible(bool visible);

    pajlada::Signals::NoArgSignal toggleBannerRequested;

    pajlada::Signals::NoArgSignal dismissed;

    void refreshLayout();

protected:
    void scaleChangedEvent(float scale) override;
    void themeChangedEvent() override;
    void showEvent(QShowEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void toggleExpansion();
    void updateScaling();
    void updateTimer();
    void updateLabelStyles();
    void scheduleInitialLayoutStabilization();

    QHBoxLayout *topLayout_{};
    QHBoxLayout *messageLayout_{};
    SvgButton *icon_{};
    ChannelView *messageView_{};
    ChannelPtr channel_{};
    QLabel *pinnerLabel_{};
    QLabel *timerLabel_{};
    QLabel *moreLabel_{};
    SvgButton *unpinButton_{};
    SvgButton *toggleButton_{};
    Split *split_{};

    int headerFontSize_ = 11;

    bool isExpanded_ = false;
    bool userManuallyCollapsed_ = false;
    bool hasPin_ = false;
    bool initialLayoutStabilizationQueued_ = false;
    QString dismissedPinId_;
    QString currentPinMessageId_;
    QString pinnerName_;

    std::optional<QDateTime> endsAt_;
    std::optional<QDateTime> pinnedAt_;
    TwitchChannel *twitchChannel_{};
    QTimer *countdownTimer_{};

    std::vector<pajlada::Signals::ScopedConnection> managedConnections_;
};

}  // namespace chatterino

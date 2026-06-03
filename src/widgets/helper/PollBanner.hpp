#pragma once

#include "providers/twitch/TwitchChannel.hpp"
#include "widgets/BaseWidget.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>

namespace chatterino {

class Split;
class SvgButton;

class PollBanner : public BaseWidget
{
    Q_OBJECT

public:
    explicit PollBanner(Split *split, QWidget *parent = nullptr);

    void setPoll(const std::optional<TwitchChannel::PollEvent> &poll,
                 TwitchChannel *channel);
    bool hasPoll() const;
    void setToggleButtonVisible(bool visible);

    pajlada::Signals::NoArgSignal toggleBannerRequested;
    pajlada::Signals::NoArgSignal dismissed;

protected:
    void scaleChangedEvent(float scale) override;
    void themeChangedEvent() override;
    void showEvent(QShowEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    void updateTimer();
    void updateLayout();
    void updateLabelStyles();
    qint64 remainingPollSeconds() const;
    void openPollDialog();
    QString dismissalKey(const TwitchChannel::PollEvent &poll) const;

    SvgButton *icon_{};
    QLabel *metadataLabel_{};
    QLabel *titleLabel_{};
    QLabel *summaryLabel_{};
    QWidget *distributionBar_{};
    QLabel *timerLabel_{};
    SvgButton *closeButton_{};
    SvgButton *toggleButton_{};
    QVBoxLayout *rootLayout_{};
    QHBoxLayout *topLayout_{};
    QVBoxLayout *contentLayout_{};
    Split *split_{};

    int headerFontSize_{11};
    int titleFontSize_{13};
    int summaryFontSize_{11};

    TwitchChannel *twitchChannel_{};
    std::optional<TwitchChannel::PollEvent> poll_;
    QDateTime pollSnapshotAt_;
    QTimer *updateTimer_{};
    std::vector<double> previousFractions_;
    std::vector<double> targetFractions_;
    std::vector<double> currentFractions_;
    QVariantAnimation *anim_{};
    QString dismissedPollKey_;
    bool expiryRefreshQueued_ = false;
    std::vector<pajlada::Signals::ScopedConnection> managedConnections_;
};

}  // namespace chatterino

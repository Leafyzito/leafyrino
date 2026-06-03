#pragma once

#include "common/Channel.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "widgets/BaseWidget.hpp"
#include "widgets/buttons/SvgButton.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QHBoxLayout>
#include <QLabel>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include <vector>

namespace chatterino {

class Split;

class PredictionBanner : public BaseWidget
{
    Q_OBJECT

public:
    explicit PredictionBanner(Split *split, QWidget *parent = nullptr);

    void setPrediction(
        const std::optional<TwitchChannel::PredictionEvent> &prediction,
        TwitchChannel *channel);

    bool hasPrediction() const;

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
    QDateTime predictionEndsAt() const;
    void updateTimer();
    void updateLayout();
    void updateLabelStyles();
    void openPredictionDialog();
    QString dismissalKey(
        const TwitchChannel::PredictionEvent &prediction) const;

    SvgButton *icon_{};
    QLabel *metadataLabel_{};
    QLabel *titleLabel_{};
    QLabel *leftOutcomeLabel_{};
    QLabel *rightOutcomeLabel_{};
    QWidget *outcomeBar_{};
    QLabel *timerLabel_{};
    SvgButton *closeButton_{};
    SvgButton *toggleButton_{};
    QVBoxLayout *rootLayout_{};
    QHBoxLayout *topLayout_{};
    QVBoxLayout *contentLayout_{};
    QHBoxLayout *outcomeRowLayout_{};
    Split *split_{};

    int headerFontSize_{11};
    int titleFontSize_{13};
    int outcomeFontSize_{12};

    TwitchChannel *twitchChannel_{};
    std::optional<TwitchChannel::PredictionEvent> prediction_;
    QTimer *updateTimer_{};
    QString dismissedPredictionKey_;

    std::vector<double> previousFractions_;
    std::vector<double> targetFractions_;
    std::vector<double> currentFractions_;
    QVariantAnimation *anim_{};

    std::vector<pajlada::Signals::ScopedConnection> managedConnections_;
};

}  // namespace chatterino

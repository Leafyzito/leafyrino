#pragma once

#include "providers/moltorino/MoltorinoFeatureFlags.hpp"

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS

#    include "widgets/DraggablePopup.hpp"

#    include <pajlada/signals/scoped-connection.hpp>
#    include <QPointer>
#    include <QVector>

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace chatterino {

class Button;
class ChannelPointsChartView;
class SvgButton;
class TwitchChannel;

class ChannelPointsChartDialog : public DraggablePopup
{
public:
    ChannelPointsChartDialog(TwitchChannel *channel, QWidget *parent = nullptr);

    static void showDialog(TwitchChannel *channel, QWidget *parent = nullptr);

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    void showEvent(QShowEvent *event) override;

private:
    void refreshHeader();
    void refreshStyle();
    void reloadChart();
    void applySizeConstraints();
    void scheduleUnpinParentOnClose(QWidget *parent);

    TwitchChannel *channel_{};
    QVBoxLayout *mainLayout_{};
    QWidget *headerWidget_{};
    QLabel *headerTitleLabel_{};
    QLabel *headerSubtitleLabel_{};
    Button *pinButton_{};
    SvgButton *closeButton_{};
    ChannelPointsChartView *chartView_{};
    bool parentUnpinScheduled_ = false;

    pajlada::Signals::ScopedConnection channelPointsConnection_;

    static QVector<QPointer<ChannelPointsChartDialog>> activeDialogs_;
};

}  // namespace chatterino

#endif

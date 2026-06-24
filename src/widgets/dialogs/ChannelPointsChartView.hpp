#pragma once

#include "providers/moltorino/MoltorinoFeatureFlags.hpp"

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS

#    include "controllers/channelpoints/ChannelPointsChartStore.hpp"

#    include <QWidget>

namespace chatterino {

class Theme;

class ChannelPointsChartView : public QWidget
{
    Q_OBJECT

public:
    explicit ChannelPointsChartView(QWidget *parent = nullptr);

    void setSamples(const QVector<ChannelPointsChartSample> &samples);
    void reloadFromStore(const QString &channelId);
    void applyTheme(const Theme &theme);
    void resetTimeRange();

    ~ChannelPointsChartView() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    class Private;
    Private *d_;
};

}  // namespace chatterino

#endif

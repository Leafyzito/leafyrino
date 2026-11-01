#pragma once

#include "widgets/BaseWidget.hpp"

#include <pajlada/signals/signalholder.hpp>

class QComboBox;
class QShowEvent;
namespace chatterino {

class ChannelView;

class AutoModReviewBar final : public BaseWidget
{
    Q_OBJECT

public:
    AutoModReviewBar(QWidget *parent, ChannelView *view);

    void setActive(bool active);
    void setSelectedChannel(QString channel);
    QString selectedChannel() const;

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    void showEvent(QShowEvent *event) override;

private:
    void refresh();
    void applyFilter();

    ChannelView *view_;
    QComboBox *channel_;
    bool active_ = false;
    pajlada::Signals::SignalHolder signalHolder_;
};

}

#pragma once

#include <pajlada/signals/signalholder.hpp>
#include <QListWidget>

namespace chatterino {

class KickAccountSwitchWidget : public QListWidget
{
public:
    explicit KickAccountSwitchWidget(QWidget *parent = nullptr);

    void refresh();

private:
    void refreshItems();

    pajlada::Signals::SignalHolder managedConnections_;
};

}

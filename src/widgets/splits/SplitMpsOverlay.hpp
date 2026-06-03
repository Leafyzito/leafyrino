// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWidget.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QLabel>
#include <QRect>
#include <QTimer>

#include <chrono>
#include <deque>

namespace chatterino {

class SplitMpsOverlay final : public BaseWidget
{
    Q_OBJECT

public:
    explicit SplitMpsOverlay(QWidget *parent);

    /// Should be called when a message is added to this split's channel view.
    void onMessageAdded();

    /// Update the rectangle of the ChannelView the overlay should be anchored to.
    void setViewRect(const QRect &rect);

protected:
    void themeChangedEvent() override;

private:
    void updateOverlay();
    void updateGeometryForCorner();

    QLabel *label_{};
    QTimer timer_;
    QRect viewRect_{};
    std::deque<std::chrono::steady_clock::time_point> timestamps_;

    pajlada::Signals::SignalHolder signalHolder_;
};

}  // namespace chatterino

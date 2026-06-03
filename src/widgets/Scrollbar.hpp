// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWidget.hpp"
#include "widgets/helper/ScrollbarHighlight.hpp"

#include <boost/circular_buffer.hpp>
#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QPropertyAnimation>
#include <QWidget>

namespace chatterino {

class ChannelView;

class Scrollbar : public BaseWidget
{
    Q_OBJECT

public:
    Scrollbar(size_t messagesLimit, ChannelView *parent);

    boost::circular_buffer<ScrollbarHighlight> getHighlights() const;
    void addHighlight(ScrollbarHighlight highlight);
    void addHighlightsAtStart(
        const std::vector<ScrollbarHighlight> &highlights_);
    void replaceHighlight(size_t index, ScrollbarHighlight replacement);

    void clearHighlights();

    void scrollToBottom(bool animate = false);
    void scrollToTop(bool animate = false);
    bool isAtBottom() const;

    qreal getMaximum() const;
    void setMaximum(qreal value);
    void offsetMaximum(qreal value);

    qreal getMinimum() const;
    void setMinimum(qreal value);
    void offsetMinimum(qreal value);

    void resetBounds();

    qreal getPageSize() const;
    void setPageSize(qreal value);

    qreal getDesiredValue() const;
    void setDesiredValue(qreal value, bool animated = false);

    qreal getBottom() const;
    qreal getCurrentValue() const;

    qreal getRelativeCurrentValue() const;

    void setHideThumb(bool hideThumb);

    bool shouldShowThumb() const;

    void setHideHighlights(bool hideHighlights);

    bool shouldShowHighlights() const;

    bool shouldHandleMouseEvents() const;

    void offset(qreal value);
    pajlada::Signals::NoArgSignal &getCurrentValueChanged();
    pajlada::Signals::NoArgSignal &getDesiredValueChanged();
    void setCurrentValue(qreal value);

    void printCurrentState(
        const QString &prefix = QStringLiteral("Scrollbar")) const;

    Q_PROPERTY(qreal desiredValue_ READ getDesiredValue WRITE setDesiredValue)

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    Q_PROPERTY(qreal currentValue_ READ getCurrentValue WRITE setCurrentValue)

    void updateScroll();

    enum class MouseLocation {

        Outside,

        AboveThumb,

        InsideThumb,

        BelowThumb,
    };

    MouseLocation locationOfMouseEvent(QMouseEvent *event) const;

    QPropertyAnimation currentValueAnimation_;

    boost::circular_buffer<ScrollbarHighlight> highlights_;

    bool atBottom_{true};

    bool hideThumb{false};

    bool settingHideThumb{false};

    bool hideHighlights = false;

    bool settingHideHighlights{false};

    MouseLocation mouseOverLocation_ = MouseLocation::Outside;
    MouseLocation mouseDownLocation_ = MouseLocation::Outside;
    QPoint lastMousePosition_;

    int trackHeight_ = 100;

    QRect thumbRect_;

    qreal maximum_ = 0;
    qreal minimum_ = 0;
    qreal pageSize_ = 0;
    qreal desiredValue_ = 0;
    qreal currentValue_ = 0;

    pajlada::Signals::NoArgSignal currentValueChanged_;
    pajlada::Signals::NoArgSignal desiredValueChanged_;

    pajlada::Signals::SignalHolder signalHolder;
};

}

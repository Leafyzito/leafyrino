// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

class QPoint;
class QWidget;

#include <QObject>
#include <QtGlobal>

namespace chatterino {

#ifdef USEWINSDK

class TitleBarButton;
class TitleBarButtons : QObject
{
public:
    TitleBarButtons(QWidget *window, TitleBarButton *minButton,
                    TitleBarButton *maxButton, TitleBarButton *closeButton);

    void hover(size_t ht, QPoint at);

    void leave();

    void mousePress(size_t ht, QPoint at);

    void mouseRelease(size_t ht, QPoint at);

    void updateMaxButton();

    void setSmallSize();

    void setRegularSize();

private:
    TitleBarButton *buttonForHt(size_t ht) const;

    QWidget *window_ = nullptr;
    TitleBarButton *minButton_ = nullptr;
    TitleBarButton *maxButton_ = nullptr;
    TitleBarButton *closeButton_ = nullptr;
};

#endif

}  // namespace chatterino

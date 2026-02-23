// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWidget.hpp"

#include <QColor>

namespace chatterino {

class ColorSwatch : public BaseWidget
{
    Q_OBJECT

public:
    explicit ColorSwatch(QWidget *parent = nullptr);

    void setColor(const QColor &color);

protected:
    void scaleChangedEvent(float newScale) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void updateSize();
    QColor color_;
};

}  // namespace chatterino

// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/ColorSwatch.hpp"

#include "singletons/Theme.hpp"

#include <QPainter>

namespace chatterino {

ColorSwatch::ColorSwatch(QWidget *parent)
    : BaseWidget(parent)
    , color_()
{
    this->updateSize();
}

void ColorSwatch::setColor(const QColor &color)
{
    if (this->color_ == color)
    {
        return;
    }
    this->color_ = color;
    this->update();
}

void ColorSwatch::scaleChangedEvent(float /*newScale*/)
{
    this->updateSize();
}

void ColorSwatch::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QColor color = this->color_;
    if (!color.isValid() || color.alpha() == 0)
    {
        color = getTheme()->isLightTheme() ? QColor("#999") : QColor("#666");
    }

    painter.setBrush(color);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QRect{QPoint{0, 0}, QSize{6, 6} * this->scale()});
}

void ColorSwatch::updateSize()
{
    int s = qRound(8 * this->scale());
    this->setFixedSize(s, s);
}

}  // namespace chatterino

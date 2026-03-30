// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/PredictionPoolBar.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QRectF>

namespace chatterino {

PredictionPoolBar::PredictionPoolBar(QWidget *parent)
    : QWidget(parent)
    , leftColor_(QStringLiteral("#2F6BFF"))
    , rightColor_(QStringLiteral("#F5359E"))
{
    this->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
}

void PredictionPoolBar::setLeftFraction(double fraction)
{
    this->leftFraction_ = qBound(0.0, fraction, 1.0);
    this->update();
}

void PredictionPoolBar::paintEvent(QPaintEvent * /*event*/)
{
    const int w = this->width();
    const int h = this->height();
    if (w <= 0 || h <= 0)
    {
        return;
    }

    const qreal r = qMin(h / 2.0, w / 2.0);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QPainterPath pill;
    pill.addRoundedRect(0, 0, w, h, r, r);
    painter.setClipPath(pill);

    const qreal splitX = this->leftFraction_ * w;
    painter.fillRect(QRectF(0, 0, splitX, h), this->leftColor_);
    painter.fillRect(QRectF(splitX, 0, w - splitX, h), this->rightColor_);
}

}  // namespace chatterino

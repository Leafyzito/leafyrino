// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/PredictionPoolBar.hpp"

#include <QFontMetrics>
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

void PredictionPoolBar::setOutcomeMeta(QString leftTop, QString rightTop,
                                       QString leftBottom, QString rightBottom)
{
    this->leftTop_ = std::move(leftTop);
    this->rightTop_ = std::move(rightTop);
    this->leftBottom_ = std::move(leftBottom);
    this->rightBottom_ = std::move(rightBottom);
    this->update();
}

void PredictionPoolBar::setRounded(bool rounded)
{
    if (this->rounded_ == rounded)
    {
        return;
    }
    this->rounded_ = rounded;
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

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QFontMetrics fm(this->font());
    const int textH = fm.height();
    const int gap = 2;

    const bool hasText =
        !this->leftTop_.isEmpty() || !this->rightTop_.isEmpty() ||
        !this->leftBottom_.isEmpty() || !this->rightBottom_.isEmpty();

    QRect barRect(0, 0, w, h);
    if (hasText)
    {
        const int topY = 0;
        const int bottomY = h - textH;
        const QRect topRect(0, topY, w, textH);
        const QRect bottomRect(0, bottomY, w, textH);

        painter.setPen(this->palette().color(QPalette::WindowText));
        if (!this->leftTop_.isEmpty())
        {
            painter.drawText(topRect.adjusted(2, 0, -2, 0),
                             Qt::AlignLeft | Qt::AlignVCenter, this->leftTop_);
        }
        if (!this->rightTop_.isEmpty())
        {
            painter.drawText(topRect.adjusted(2, 0, -2, 0),
                             Qt::AlignRight | Qt::AlignVCenter,
                             this->rightTop_);
        }
        if (!this->leftBottom_.isEmpty())
        {
            painter.drawText(bottomRect.adjusted(2, 0, -2, 0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             this->leftBottom_);
        }
        if (!this->rightBottom_.isEmpty())
        {
            painter.drawText(bottomRect.adjusted(2, 0, -2, 0),
                             Qt::AlignRight | Qt::AlignVCenter,
                             this->rightBottom_);
        }

        const int barTop = textH + gap;
        const int barBottom = h - textH - gap;
        const int barH = barBottom - barTop;
        if (barH > 2)
        {
            barRect = QRect(0, barTop, w, barH);
        }
    }

    if (this->rounded_)
    {
        const qreal r = qMin(barRect.height() / 2.0, barRect.width() / 2.0);
        QPainterPath pill;
        pill.addRoundedRect(barRect, r, r);
        painter.setClipPath(pill);
    }
    else
    {
        painter.setClipRect(barRect);
    }

    const qreal splitX = barRect.left() + this->leftFraction_ * barRect.width();
    painter.fillRect(QRectF(barRect.left(), barRect.top(),
                            splitX - barRect.left(), barRect.height()),
                     this->leftColor_);
    painter.fillRect(QRectF(splitX, barRect.top(), barRect.right() + 1 - splitX,
                            barRect.height()),
                     this->rightColor_);
}

}  // namespace chatterino

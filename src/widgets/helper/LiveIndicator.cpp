// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/LiveIndicator.hpp"

#include "Application.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"
#include "util/Helpers.hpp"

#include <QFontMetrics>
#include <QPainter>
#include <QString>

namespace chatterino {

using namespace Qt::Literals::StringLiterals;

LiveIndicator::LiveIndicator(QWidget *parent)
    : BaseWidget(parent)
{
    this->setMinimumHeight(5);
    this->setMouseTracking(true);
    this->updateScale();
}

void LiveIndicator::setViewers(int viewers)
{
    this->viewers_ = viewers;
    this->setToolTip(u"Live with %1 viewers"_s.arg(localizeNumbers(viewers)));
    this->updateScale();
}

void LiveIndicator::setTextMode(bool textMode)
{
    this->textMode_ = textMode;
    this->updateScale();
}

QString LiveIndicator::displayText() const
{
    return localizeNumbers(this->viewers_);
}

void LiveIndicator::scaleChangedEvent(float)
{
    this->updateScale();
}

void LiveIndicator::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    QColor color = getTheme()->tabs.liveIndicator;

    if (this->hovered)
    {
        if (getTheme()->isLightTheme())
        {
            color = color.darker(150);
        }
        else
        {
            color = color.lighter(150);
        }
    }

    if (this->textMode_)
    {
        painter.setPen(color);
        painter.setFont(
            getApp()->getFonts()->getFont(FontStyle::UiMedium, this->scale()));
        painter.drawText(this->rect(), Qt::AlignVCenter | Qt::AlignLeft,
                         this->displayText());
        return;
    }

    painter.setBrush(color);
    painter.setPen(Qt::NoPen);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.drawEllipse(QRect{
        QPoint{0, 0},
        QSize{5, 5} * this->scale(),
    });
}

void LiveIndicator::enterEvent(QEnterEvent *)
{
    this->hovered = true;
    this->update();
}
void LiveIndicator::leaveEvent(QEvent *)
{
    this->hovered = false;
    this->update();
}

void LiveIndicator::updateScale()
{
    if (this->textMode_)
    {
        const auto font =
            getApp()->getFonts()->getFont(FontStyle::UiMedium, this->scale());
        const QFontMetrics fontMetrics(font);
        const auto text = this->displayText();
        this->setFixedWidth(fontMetrics.horizontalAdvance(text));
        this->setFixedHeight(fontMetrics.height());
    }
    else
    {
        this->setFixedWidth(qRound(6 * this->scale()));
        this->setFixedHeight(qRound(6 * this->scale()));
    }

    this->update();
}

}  // namespace chatterino

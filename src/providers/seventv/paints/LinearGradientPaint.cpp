#include "providers/seventv/paints/LinearGradientPaint.hpp"

#include <utility>

namespace chatterino {

LinearGradientPaint::LinearGradientPaint(
    QString name, QString id, std::optional<QColor> color, QGradientStops stops,
    bool repeat, float angle, std::vector<PaintDropShadow> dropShadows)
    : Paint(std::move(id))
    , name_(std::move(name))
    , color_(color)
    , stops_(std::move(stops))
    , repeat_(repeat)
    , angle_(angle)
    , dropShadows_(std::move(dropShadows))
{
}

bool LinearGradientPaint::animated() const
{
    return false;
}

QBrush LinearGradientPaint::asBrush(const QColor userColor,
                                    const QRectF drawingRect) const
{
    QPointF startPoint = drawingRect.bottomLeft();
    QPointF endPoint = drawingRect.topRight();

    const int angleStep = int(this->angle_ / 90) % 4;
    if (angleStep == 1)
    {
        startPoint = drawingRect.topLeft();
        endPoint = drawingRect.bottomRight();
    }
    if (angleStep == 2)
    {
        startPoint = drawingRect.topRight();
        endPoint = drawingRect.bottomLeft();
    }
    if (angleStep == 3)
    {
        startPoint = drawingRect.bottomRight();
        endPoint = drawingRect.topLeft();
    }

    QLineF gradientAxis;
    gradientAxis.setP1(drawingRect.center());
    gradientAxis.setAngle(90.0F - this->angle_);

    QLineF colorStartAxis;
    colorStartAxis.setP1(startPoint);
    colorStartAxis.setAngle(-this->angle_);

    QLineF colorStopAxis;
    colorStopAxis.setP1(endPoint);
    colorStopAxis.setAngle(-this->angle_);

    QPointF gradientStart;
    QPointF gradientEnd;
    gradientAxis.intersects(colorStartAxis, &gradientStart);
    gradientAxis.intersects(colorStopAxis, &gradientEnd);

    if (this->repeat_)
    {
        QLineF gradientLine(gradientStart, gradientEnd);
        gradientStart = gradientLine.pointAt(this->stops_.front().first);
        gradientEnd = gradientLine.pointAt(this->stops_.back().first);
    }

    QLinearGradient gradient(gradientStart, gradientEnd);

    auto spread =
        this->repeat_ ? QGradient::RepeatSpread : QGradient::PadSpread;
    gradient.setSpread(spread);

    for (const auto &[position, color] : this->stops_)
    {
        auto combinedColor =
            LinearGradientPaint::overlayColors(userColor, color);
        auto offsetPosition =
            this->repeat_ ? LinearGradientPaint::offsetRepeatingStopPosition(
                                position, this->stops_)
                          : position;
        gradient.setColorAt(offsetPosition, combinedColor);
    }

    return {gradient};
}

const std::vector<PaintDropShadow> &LinearGradientPaint::getDropShadows() const
{
    return this->dropShadows_;
}

}

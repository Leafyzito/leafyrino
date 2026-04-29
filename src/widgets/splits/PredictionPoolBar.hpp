// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QColor>
#include <QWidget>

namespace chatterino {

/// Single pill-shaped bar: left segment (blue) and right segment (magenta) by fraction.
class PredictionPoolBar : public QWidget
{
public:
    explicit PredictionPoolBar(QWidget *parent = nullptr);

    /// Portion for the left (first outcome) color, typically p0 / (p0 + p1). 0.5 if pool empty.
    void setLeftFraction(double fraction);

    /// Sets strings drawn above/below the bar.
    /// Typical values: top = return ratio ("2.64x"), bottom = points total ("277K").
    void setOutcomeMeta(QString leftTop, QString rightTop, QString leftBottom,
                        QString rightBottom);

    /// If false, the bar is drawn with square edges (no rounded corners).
    void setRounded(bool rounded);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    double leftFraction_{0.5};
    bool rounded_{true};
    QString leftTop_;
    QString rightTop_;
    QString leftBottom_;
    QString rightBottom_;
    const QColor leftColor_;
    const QColor rightColor_;
};

}  // namespace chatterino

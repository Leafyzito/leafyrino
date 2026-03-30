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

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    double leftFraction_{0.5};
    const QColor leftColor_;
    const QColor rightColor_;
};

}  // namespace chatterino

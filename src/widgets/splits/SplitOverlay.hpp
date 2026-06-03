// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWidget.hpp"
#include "widgets/splits/SplitCommon.hpp"

#include <QPushButton>

#include <optional>

namespace chatterino {

class Split;

enum class SplitOverlayButton {
    Move,
    Left,
    Up,
    Right,
    Down,
};

class SplitOverlay : public BaseWidget
{
public:
    explicit SplitOverlay(Split *parent);

    void setHoveredButton(std::optional<SplitOverlayButton> hoveredButton);

    void dragPressed();

    void createSplitPressed(SplitDirection direction);

protected:
    void scaleChangedEvent(float newScale) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    std::optional<SplitOverlayButton> hoveredButton_{};
    Split *split_;
    QPushButton *move_;
    QPushButton *left_;
    QPushButton *up_;
    QPushButton *right_;
    QPushButton *down_;
};

}

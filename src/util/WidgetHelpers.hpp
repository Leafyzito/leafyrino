// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

class QWidget;
class QPoint;
class QScreen;
class QRect;

namespace chatterino::widgets {

enum class BoundsChecking {

    Off,

    CursorPosition,

    DesiredPosition,
};

QRect checkInitialBounds(QRect initialBounds,
                         BoundsChecking mode = BoundsChecking::DesiredPosition);

void moveWindowTo(QWidget *window, QPoint position,
                  BoundsChecking mode = BoundsChecking::DesiredPosition);

void showAndMoveWindowTo(QWidget *window, QPoint position,
                         BoundsChecking mode = BoundsChecking::DesiredPosition);

}

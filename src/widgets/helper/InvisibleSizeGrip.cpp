// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/InvisibleSizeGrip.hpp"

namespace chatterino {

InvisibleSizeGrip::InvisibleSizeGrip(QWidget *parent)
    : QSizeGrip(parent)
{

    this->setMouseTracking(true);
}

void InvisibleSizeGrip::paintEvent(QPaintEvent *event)
{
}

}

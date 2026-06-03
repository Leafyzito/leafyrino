// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/FlagsEnum.hpp"
#include "widgets/BaseWindow.hpp"

class QDialogButtonBox;

namespace chatterino {

class BasePopup : public BaseWindow
{
public:
    explicit BasePopup(FlagsEnum<BaseWindow::Flags> flags_ = None,
                       QWidget *parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent *e) override;

    bool handleEscape(QKeyEvent *e, QDialogButtonBox *buttonBox);

    bool handleEnter(QKeyEvent *e, QDialogButtonBox *buttonBox);
};

}

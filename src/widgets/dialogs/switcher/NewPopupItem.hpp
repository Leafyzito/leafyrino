// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/dialogs/switcher/AbstractSwitcherItem.hpp"

namespace chatterino {

class NewPopupItem : public AbstractSwitcherItem
{
public:

    NewPopupItem(const QString &channelName);

    void action() override;

    void paint(QPainter *painter, const QRect &rect) const override;
    QSize sizeHint(const QRect &rect) const override;

private:
    static constexpr const char *TEXT_FORMAT =
        "Open channel \"%1\" in new popup";
    QString channelName_;
    QString text_;
};

}

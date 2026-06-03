// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QProxyStyle>

class QTableView;

namespace chatterino {

class TableRowDragStyle : public QProxyStyle
{
public:

    static void applyTo(QTableView *view);

    void drawPrimitive(QStyle::PrimitiveElement element,
                       const QStyleOption *option, QPainter *painter,
                       const QWidget *widget = nullptr) const override;

private:

    TableRowDragStyle(const QString &name);
};

}

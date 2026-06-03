// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/TableStyles.hpp"

#include <QAbstractItemView>
#include <QPainter>
#include <QStyleOption>
#include <QTableView>
#include <QWidget>

namespace chatterino {

TableRowDragStyle::TableRowDragStyle(const QString &name)
    : QProxyStyle(name)
{
}

void TableRowDragStyle::applyTo(QTableView *view)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 1, 0)
    auto styleName = view->style()->name();
#else
    QString styleName = "fusion";
#endif
    auto *proxyStyle = new TableRowDragStyle(styleName);
    proxyStyle->setParent(view);
    view->setStyle(proxyStyle);
}

void TableRowDragStyle::drawPrimitive(QStyle::PrimitiveElement element,
                                      const QStyleOption *option,
                                      QPainter *painter,
                                      const QWidget *widget) const
{
    if (element != QStyle::PE_IndicatorItemViewItemDrop)
    {
        QProxyStyle::drawPrimitive(element, option, painter, widget);
        return;
    }

    const auto *view = dynamic_cast<const QAbstractItemView *>(widget);
    if (!view)
    {
        assert(false && "TableStyle must be used on a QAbstractItemView");
        return;
    }

    if (option->rect.isNull())
    {
        return;
    }

    auto selected = view->currentIndex();
    auto hovered = view->indexAt(option->rect.center());
    if (!selected.isValid() || !hovered.isValid())
    {

        assert(false && "Got bad indices");
        return;
    }

    int y = option->rect.top();
    if (hovered.row() >= selected.row())
    {
        y = option->rect.bottom();
    }

    painter->setPen({Qt::white, 2});
    painter->drawLine(0, y, widget->width(), y);
}

}

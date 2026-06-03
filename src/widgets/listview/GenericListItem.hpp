// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QIcon>
#include <QPainter>
#include <QRect>
#include <QVariant>

namespace chatterino {

class GenericListItem
{
public:
    static GenericListItem *fromVariant(const QVariant &variant);

    virtual ~GenericListItem() = default;

    GenericListItem();

    GenericListItem(const QIcon &icon);

    virtual void action() = 0;

    virtual void paint(QPainter *painter, const QRect &rect) const = 0;
    virtual QSize sizeHint(const QRect &rect) const = 0;

protected:
    QIcon icon_;
    static const QSize ICON_SIZE;
};

}  // namespace chatterino

Q_DECLARE_METATYPE(chatterino::GenericListItem *);

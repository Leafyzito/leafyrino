// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QFont>
#include <QListWidget>
#include <QSpinBox>
#include <QWidget>

namespace chatterino {

class IntItem;

class FontSizeWidget : public QWidget
{
    Q_OBJECT

public:
    FontSizeWidget(const QFont &startFont, QWidget *parent = nullptr);

    int getSelected() const;

Q_SIGNALS:
    void selectedChanged();

private:
    void setListSelected(int size);

    IntItem *customItem;
    QListWidget *list;
    QSpinBox *edit;
};

}

// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QFont>
#include <QListWidget>
#include <QString>
#include <QWidget>

namespace chatterino {

class FontWeightWidget : public QWidget
{
    Q_OBJECT

public:
    FontWeightWidget(const QFont &startFont, QWidget *parent = nullptr);

    void setFamily(const QString &family);

    int getSelected() const;

Q_SIGNALS:
    void selectedChanged();

private:
    QListWidget *list;
};

}  // namespace chatterino

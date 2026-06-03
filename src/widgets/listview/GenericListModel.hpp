// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/listview/GenericListItem.hpp"

#include <QAbstractListModel>
#include <QObject>

#include <memory>
#include <vector>

namespace chatterino {

class GenericListModel : public QAbstractListModel
{
public:
    GenericListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;

    QVariant data(const QModelIndex &index, int role) const override;

    void addItem(std::unique_ptr<GenericListItem> item);

    void clear();

    void reserve(size_t capacity);

private:
    std::vector<std::unique_ptr<GenericListItem>> items_;
};
}

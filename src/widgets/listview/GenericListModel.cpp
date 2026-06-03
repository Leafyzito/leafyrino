// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/listview/GenericListModel.hpp"

namespace chatterino {

GenericListModel::GenericListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int GenericListModel::rowCount(const QModelIndex & ) const
{
    return this->items_.size();
}

QVariant GenericListModel::data(const QModelIndex &index, int ) const
{
    if (!index.isValid())
    {
        return {};
    }

    if (index.row() >= static_cast<int>(this->items_.size()))
    {
        return {};
    }

    auto *item = this->items_[index.row()].get();

    return QVariant::fromValue(static_cast<void *>(item));
}

void GenericListModel::addItem(std::unique_ptr<GenericListItem> item)
{

    this->beginInsertRows(QModelIndex(), this->items_.size(),
                          this->items_.size());
    this->items_.push_back(std::move(item));
    this->endInsertRows();
}

void GenericListModel::clear()
{
    if (this->items_.empty())
    {
        return;
    }

    this->beginRemoveRows(QModelIndex(), 0, this->items_.size() - 1);

    this->items_.clear();

    this->endRemoveRows();
}

void GenericListModel::reserve(size_t capacity)
{
    this->items_.reserve(capacity);
}

}

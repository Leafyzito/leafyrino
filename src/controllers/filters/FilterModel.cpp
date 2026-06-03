// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/filters/FilterModel.hpp"

#include "Application.hpp"
#include "controllers/filters/FilterRecord.hpp"
#include "singletons/Settings.hpp"
#include "util/StandardItemHelper.hpp"

namespace chatterino {

FilterModel::FilterModel(QObject *parent)
    : SignalVectorModel<FilterRecordPtr>(3, parent)
{
}

FilterRecordPtr FilterModel::getItemFromRow(std::vector<QStandardItem *> &row,
                                            const FilterRecordPtr &original)
{
    auto item =
        std::make_shared<FilterRecord>(row[0]->data(Qt::DisplayRole).toString(),
                                       row[1]->data(Qt::DisplayRole).toString(),
                                       original->getId());

    setBoolItem(row[2], item->valid(), false, false);
    setStringItem(row[2], item->valid() ? "Valid" : "Show errors");

    return item;
}

void FilterModel::getRowFromItem(const FilterRecordPtr &item,
                                 std::vector<QStandardItem *> &row)
{
    setStringItem(row[0], item->getName());
    setStringItem(row[1], item->getFilter());
    setBoolItem(row[2], item->valid(), false, false);
    setStringItem(row[2], item->valid() ? "Valid" : "Show errors");
}

}

// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/ignores/IgnoreModel.hpp"

#include "Application.hpp"
#include "controllers/ignores/IgnorePhrase.hpp"
#include "singletons/Settings.hpp"
#include "util/StandardItemHelper.hpp"

namespace chatterino {

IgnoreModel::IgnoreModel(QObject *parent)
    : SignalVectorModel<IgnorePhrase>(5, parent)
{
}

IgnorePhrase IgnoreModel::getItemFromRow(std::vector<QStandardItem *> &row,
                                         const IgnorePhrase &original)
{
    return IgnorePhrase{row[0]->data(Qt::DisplayRole).toString(),
                        row[1]->data(Qt::CheckStateRole).toBool(),
                        row[3]->data(Qt::CheckStateRole).toBool(),
                        row[4]->data(Qt::DisplayRole).toString(),
                        row[2]->data(Qt::CheckStateRole).toBool()};
}

void IgnoreModel::getRowFromItem(const IgnorePhrase &item,
                                 std::vector<QStandardItem *> &row)
{
    setStringItem(row[0], item.getPattern());
    setBoolItem(row[1], item.isRegex());
    setBoolItem(row[2], item.isCaseSensitive());
    setBoolItem(row[3], item.isBlock());
    setStringItem(row[4], item.getReplace());
}

}  // namespace chatterino

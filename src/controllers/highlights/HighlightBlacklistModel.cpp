// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/highlights/HighlightBlacklistModel.hpp"

#include "Application.hpp"
#include "controllers/highlights/HighlightBlacklistUser.hpp"
#include "singletons/Settings.hpp"
#include "util/StandardItemHelper.hpp"

namespace chatterino {

HighlightBlacklistModel::HighlightBlacklistModel(QObject *parent)
    : SignalVectorModel<HighlightBlacklistUser>(2, parent)
{
}

HighlightBlacklistUser HighlightBlacklistModel::getItemFromRow(
    std::vector<QStandardItem *> &row, const HighlightBlacklistUser &original)
{
    return HighlightBlacklistUser{
        row[Column::Pattern]->data(Qt::DisplayRole).toString(),
        row[Column::UseRegex]->data(Qt::CheckStateRole).toBool()};
}

void HighlightBlacklistModel::getRowFromItem(const HighlightBlacklistUser &item,
                                             std::vector<QStandardItem *> &row)
{
    setStringItem(row[Column::Pattern], item.getPattern());
    setBoolItem(row[Column::UseRegex], item.isRegex());
}

}  // namespace chatterino

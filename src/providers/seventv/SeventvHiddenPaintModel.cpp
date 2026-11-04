// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/seventv/SeventvHiddenPaintModel.hpp"

#include "util/StandardItemHelper.hpp"

namespace chatterino {

SeventvHiddenPaintModel::SeventvHiddenPaintModel(QObject *parent)
    : SignalVectorModel<SeventvHiddenPaint>(Column::COUNT, parent)
{
}

SeventvHiddenPaint SeventvHiddenPaintModel::getItemFromRow(
    std::vector<QStandardItem *> & /*row*/, const SeventvHiddenPaint &original,
    std::optional<QModelIndex> /*index*/, std::optional<int> /*role*/)
{
    // Rows aren't editable, so there's nothing to read back.
    return original;
}

void SeventvHiddenPaintModel::getRowFromItem(const SeventvHiddenPaint &item,
                                             std::vector<QStandardItem *> &row)
{
    setStringItem(row[Column::Name], item.name, false);
    setStringItem(row[Column::ID], item.id, false);
}

}  // namespace chatterino

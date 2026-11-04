// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/SignalVectorModel.hpp"
#include "providers/seventv/SeventvHiddenPaint.hpp"

#include <QObject>

namespace chatterino {

class SeventvHiddenPaintModel : public SignalVectorModel<SeventvHiddenPaint>
{
public:
    explicit SeventvHiddenPaintModel(QObject *parent);

    enum Column {
        Name,
        ID,
        COUNT,
    };

protected:
    SeventvHiddenPaint getItemFromRow(std::vector<QStandardItem *> &row,
                                      const SeventvHiddenPaint &original,
                                      std::optional<QModelIndex> index,
                                      std::optional<int> role) override;

    void getRowFromItem(const SeventvHiddenPaint &item,
                        std::vector<QStandardItem *> &row) override;
};

}  // namespace chatterino

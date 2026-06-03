// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/SignalVectorModel.hpp"

#include <QObject>

namespace chatterino {

class HighlightBlacklistUser;
class HighlightController;

class HighlightBlacklistModel : public SignalVectorModel<HighlightBlacklistUser>
{
public:
    explicit HighlightBlacklistModel(QObject *parent);

    enum Column {
        Pattern = 0,
        UseRegex = 1,
    };

protected:

    HighlightBlacklistUser getItemFromRow(
        std::vector<QStandardItem *> &row,
        const HighlightBlacklistUser &original) override;

    void getRowFromItem(const HighlightBlacklistUser &item,
                        std::vector<QStandardItem *> &row) override;
};

}

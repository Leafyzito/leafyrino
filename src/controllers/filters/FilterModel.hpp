// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/SignalVectorModel.hpp"

#include <QObject>

namespace chatterino {

class FilterRecord;
using FilterRecordPtr = std::shared_ptr<FilterRecord>;

class FilterModel : public SignalVectorModel<FilterRecordPtr>
{
public:
    explicit FilterModel(QObject *parent);

protected:

    FilterRecordPtr getItemFromRow(std::vector<QStandardItem *> &row,
                                   const FilterRecordPtr &original) override;

    void getRowFromItem(const FilterRecordPtr &item,
                        std::vector<QStandardItem *> &row) override;
};

}

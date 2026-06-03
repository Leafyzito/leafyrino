// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/SignalVectorModel.hpp"

#include <QObject>

namespace chatterino {

class IgnorePhrase;

class IgnoreModel : public SignalVectorModel<IgnorePhrase>
{
public:
    explicit IgnoreModel(QObject *parent);

protected:
    IgnorePhrase getItemFromRow(std::vector<QStandardItem *> &row,
                                const IgnorePhrase &original) override;

    void getRowFromItem(const IgnorePhrase &item,
                        std::vector<QStandardItem *> &row) override;
};

}  // namespace chatterino

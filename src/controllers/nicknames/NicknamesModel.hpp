// SPDX-FileCopyrightText: 2021 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/SignalVectorModel.hpp"

#include <QObject>

#include <vector>

namespace chatterino {

class Nickname;

class NicknamesModel : public SignalVectorModel<Nickname>
{
public:
    explicit NicknamesModel(QObject *parent);

protected:
    Nickname getItemFromRow(std::vector<QStandardItem *> &row,
                            const Nickname &original) override;

    void getRowFromItem(const Nickname &item,
                        std::vector<QStandardItem *> &row) override;
};

}  // namespace chatterino

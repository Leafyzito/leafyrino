// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/SignalVectorModel.hpp"
#include "util/QStringHash.hpp"

#include <unordered_map>

namespace chatterino {

class Account;
class AccountController;

class AccountModel : public SignalVectorModel<std::shared_ptr<Account>>
{
public:
    AccountModel(QObject *parent);

protected:

    std::shared_ptr<Account> getItemFromRow(
        std::vector<QStandardItem *> &row,
        const std::shared_ptr<Account> &original) override;

    void getRowFromItem(const std::shared_ptr<Account> &item,
                        std::vector<QStandardItem *> &row) override;

    int beforeInsert(const std::shared_ptr<Account> &item,
                     std::vector<QStandardItem *> &row,
                     int proposedIndex) override;

    void afterRemoved(const std::shared_ptr<Account> &item,
                      std::vector<QStandardItem *> &row, int index) override;

    friend class AccountController;

private:
    std::unordered_map<QString, int> categoryCount_;
};

}

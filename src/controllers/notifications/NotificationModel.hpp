// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/SignalVectorModel.hpp"

#include <QObject>

namespace chatterino {

class NotificationController;

class NotificationModel : public SignalVectorModel<QString>
{
    explicit NotificationModel(QObject *parent);

protected:

    QString getItemFromRow(std::vector<QStandardItem *> &row,
                           const QString &original) override;

    void getRowFromItem(const QString &item,
                        std::vector<QStandardItem *> &row) override;

    friend class NotificationController;
};

}

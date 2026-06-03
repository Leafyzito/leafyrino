// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/SignalVectorModel.hpp"
#include "controllers/logging/ChannelLog.hpp"

#include <QObject>

namespace chatterino {

class ChannelLoggingModel : public SignalVectorModel<ChannelLog>
{
    explicit ChannelLoggingModel(QObject *parent);

    enum Column {
        Channel,
        COUNT,
    };

protected:

    ChannelLog getItemFromRow(std::vector<QStandardItem *> &row,
                              const ChannelLog &original) override;

    void getRowFromItem(const ChannelLog &item,
                        std::vector<QStandardItem *> &row) override;

    friend class ModerationPage;
};

}

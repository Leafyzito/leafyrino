// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>

namespace chatterino {

struct ChannelPointsChartSample {
    QDateTime time;
    qint64 balance = 0;
};

class ChannelPointsChartStore
{
public:
    ChannelPointsChartStore() = delete;

    static void recordSample(const QString &channelId, qint64 balance);
    static QVector<ChannelPointsChartSample> samplesForChannel(
        const QString &channelId);
};

}  // namespace chatterino

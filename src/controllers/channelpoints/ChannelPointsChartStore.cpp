// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/channelpoints/ChannelPointsChartStore.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "common/QLogging.hpp"
#include "singletons/Paths.hpp"
#include "util/CombinePath.hpp"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>

#include <algorithm>

namespace {

using namespace chatterino;
using namespace literals;

const auto CHART_FILE_NAME = u"channel-points-chart.json"_s;

struct ChartDataCache {
    QMutex mutex;
    QJsonObject data;
    bool loaded = false;
};

ChartDataCache &chartCache()
{
    static ChartDataCache cache;
    return cache;
}

QString chartFilePath()
{
    return combinePath(getApp()->getPaths().miscDirectory, CHART_FILE_NAME);
}

void ensureChartDataLoaded(ChartDataCache &cache, bool forceReload = false)
{
    if (cache.loaded && !forceReload)
    {
        return;
    }

    cache.loaded = true;
    QFile file(chartFilePath());
    if (!file.open(QIODevice::ReadOnly))
    {
        return;
    }

    const auto document = QJsonDocument::fromJson(file.readAll());
    if (document.isObject())
    {
        cache.data = document.object();
    }
}

bool lastSampleHasBalance(const QJsonArray &samples, qint64 balance)
{
    if (samples.isEmpty())
    {
        return false;
    }

    const auto last = samples.last().toObject();
    return last.value("balance"_L1).toInteger() == balance;
}

QVector<ChannelPointsChartSample> parseSamples(const QJsonArray &samples)
{
    QVector<ChannelPointsChartSample> result;
    result.reserve(samples.size());

    for (const auto &value : samples)
    {
        if (!value.isObject())
        {
            continue;
        }

        const auto object = value.toObject();
        const auto time =
            QDateTime::fromString(object.value("t"_L1).toString(),
                                  Qt::ISODateWithMs);
        if (!time.isValid())
        {
            continue;
        }

        const auto balanceValue = object.value("balance"_L1);
        if (!balanceValue.isDouble())
        {
            continue;
        }

        result.append({time.toUTC(), balanceValue.toInteger()});
    }

    std::sort(result.begin(), result.end(),
              [](const auto &left, const auto &right) {
                  return left.time < right.time;
              });

    return result;
}

}  // namespace

namespace chatterino {

void ChannelPointsChartStore::recordSample(const QString &channelId,
                                           qint64 balance)
{
    if (channelId.isEmpty() || balance < 0)
    {
        return;
    }

    auto &cache = chartCache();
    QMutexLocker locker(&cache.mutex);

    ensureChartDataLoaded(cache);

    auto samples = cache.data.value(channelId).toArray();
    if (lastSampleHasBalance(samples, balance))
    {
        return;
    }

    samples.append(QJsonObject{
        {"t"_L1,
         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"balance"_L1, balance},
    });
    cache.data.insert(channelId, samples);

    QSaveFile file(chartFilePath());
    if (!file.open(QIODevice::WriteOnly))
    {
        qCWarning(chatterinoTwitch)
            << "[Points] Failed to open channel points chart file for writing:"
            << file.errorString();
        return;
    }

    file.write(QJsonDocument(cache.data).toJson(QJsonDocument::Indented));
    if (!file.commit())
    {
        qCWarning(chatterinoTwitch)
            << "[Points] Failed to save channel points chart file:"
            << file.errorString();
    }
}

QVector<ChannelPointsChartSample> ChannelPointsChartStore::samplesForChannel(
    const QString &channelId)
{
    if (channelId.isEmpty())
    {
        return {};
    }

    auto &cache = chartCache();
    QMutexLocker locker(&cache.mutex);

    ensureChartDataLoaded(cache, true);
    return parseSamples(cache.data.value(channelId).toArray());
}

}  // namespace chatterino

// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/DebugCount.hpp"

#include "common/UniqueAccess.hpp"
#include "messages/Image.hpp"
#include "util/QMagicEnum.hpp"

#include <magic_enum/magic_enum.hpp>
#include <QLocale>
#include <QStringBuilder>

#include <algorithm>
#include <array>
#include <numeric>

namespace {

using namespace chatterino;

struct Count {
    int64_t value = 0;
};

UniqueAccess<std::array<Count, static_cast<size_t>(DebugObject::Count)>> COUNTS;

constexpr bool isBytes(DebugObject target)
{
    switch (target)
    {
        default:
            return false;

        case DebugObject::BytesImageCurrent:
        case DebugObject::BytesImageLoaded:
        case DebugObject::BytesImageUnloaded:
            return true;
    }
}

}

namespace chatterino {

void DebugCount::set(DebugObject target, int64_t amount)
{
    auto counts = COUNTS.access();

    auto &it = counts->at(static_cast<size_t>(target));
    it.value = amount;
}

void DebugCount::increase(DebugObject target, int64_t amount)
{
    auto counts = COUNTS.access();

    auto &it = counts->at(static_cast<size_t>(target));
    it.value += amount;
}

void DebugCount::decrease(DebugObject target, int64_t amount)
{
    auto counts = COUNTS.access();

    auto &it = counts->at(static_cast<size_t>(target));
    it.value -= amount;
}

QString DebugCount::getDebugText()
{
    static const QLocale locale(QLocale::English);

    QString text;
    {
        auto counts = COUNTS.access();

        for (size_t key = 0; key < static_cast<size_t>(DebugObject::Count);
             key++)
        {
            auto &count = counts->at(key);

            QString formatted;
            if (isBytes(static_cast<DebugObject>(key)))
            {
                formatted = locale.formattedDataSize(count.value);
            }
            else
            {
                formatted =
                    locale.toString(static_cast<qlonglong>(count.value));
            }

            text += qmagicenum::enumName(static_cast<DebugObject>(key)) %
                    ": " % formatted % '\n';
        }
    }

#ifndef DISABLE_IMAGE_EXPIRATION_POOL
    const auto providerUsage =
        ImageExpirationPool::instance().getProviderUsageSnapshot();
    const auto providerBytes =
        std::accumulate(providerUsage.begin(), providerUsage.end(), int64_t{0},
                        [](int64_t total, const auto &usage) {
                            return total + usage.bytes;
                        });

    if (providerBytes > 0)
    {
        text += "\ntracked image provider bytes:\n";

        constexpr size_t MAX_PROVIDERS = 8;
        for (size_t i = 0; i < providerUsage.size() && i < MAX_PROVIDERS; ++i)
        {
            const auto &usage = providerUsage[i];
            const auto percent =
                (static_cast<double>(usage.bytes) * 100.0) /
                static_cast<double>(providerBytes);

            text += QStringLiteral("  ") % usage.provider %
                    QStringLiteral(": ") %
                    locale.formattedDataSize(usage.bytes) %
                    QStringLiteral(" (") %
                    QString::number(percent, 'f', 1) % QStringLiteral("%, ") %
                    locale.toString(static_cast<qlonglong>(usage.images)) %
                    QStringLiteral(" img");
            if (usage.animatedImages > 0)
            {
                text += QStringLiteral(", ") %
                        locale.toString(
                            static_cast<qlonglong>(usage.animatedImages)) %
                        QStringLiteral(" anim");
            }
            text += QStringLiteral(")\n");
        }
    }
#endif

    return text;
}

}

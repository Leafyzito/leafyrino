// SPDX-FileCopyrightText: 2021 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/network/NetworkCommon.hpp"

#include <QStringList>

namespace chatterino {

std::vector<std::pair<QByteArray, QByteArray>> parseHeaderList(
    const QString &headerListString)
{
    std::vector<std::pair<QByteArray, QByteArray>> res;

    auto headerPairs = headerListString.split(";");

    for (const auto &headerPair : headerPairs)
    {
        const auto headerName =
            headerPair.section(":", 0, 0).trimmed().toUtf8();
        const auto headerValue = headerPair.section(":", 1).trimmed().toUtf8();

        if (headerName.isEmpty() || headerValue.isEmpty())
        {
            continue;
        }

        res.emplace_back(headerName, headerValue);
    }

    return res;
}

}  // namespace chatterino

// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

#include <functional>
#include <vector>

class QNetworkReply;

namespace chatterino {

class NetworkResult;

using NetworkSuccessCallback = std::function<void(NetworkResult)>;
using NetworkErrorCallback = std::function<void(NetworkResult)>;
using NetworkFinallyCallback = std::function<void()>;

enum class NetworkRequestType {
    Get,
    Post,
    Put,
    Delete,
    Patch,
};

std::vector<std::pair<QByteArray, QByteArray>> parseHeaderList(
    const QString &headerListString);

}  // namespace chatterino

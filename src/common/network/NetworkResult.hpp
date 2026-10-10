// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkReply>
#include <rapidjson/document.h>

#include <optional>

namespace chatterino {

class NetworkResult
{
public:
    using NetworkError = QNetworkReply::NetworkError;

    NetworkResult(NetworkError error, const QVariant &httpStatusCode,
                  QByteArray data, QByteArray etag = {});

    QJsonObject parseJson() const;

    QJsonArray parseJsonArray() const;

    QJsonValue parseJsonValue() const;

    rapidjson::Document parseRapidJson() const;
    const QByteArray &getData() const;
    const QByteArray &etag() const;

    NetworkError error() const
    {
        return this->error_;
    }

    std::optional<int> status() const
    {
        return this->status_;
    }

    QString formatError() const;

private:
    QByteArray data_;
    QByteArray etag_;

    NetworkError error_;
    std::optional<int> status_;
};

}  // namespace chatterino

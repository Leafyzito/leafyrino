// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/network/NetworkCommon.hpp"

#include <QHttpMultiPart>

#include <memory>

class QJsonArray;
class QJsonObject;
class QJsonDocument;

namespace chatterino {

class NetworkData;

class NetworkRequest final
{
    std::shared_ptr<NetworkData> data;

    bool executed_ = false;

public:
    explicit NetworkRequest(
        const std::string &url,
        NetworkRequestType requestType = NetworkRequestType::Get);
    explicit NetworkRequest(const QUrl &url, NetworkRequestType requestType =
                                                 NetworkRequestType::Get);

    NetworkRequest(NetworkRequest &&other) = default;
    NetworkRequest &operator=(NetworkRequest &&other) = default;

    NetworkRequest(const NetworkRequest &other) = delete;
    NetworkRequest &operator=(const NetworkRequest &other) = delete;

    ~NetworkRequest();

    NetworkRequest type(NetworkRequestType newRequestType) &&;

    NetworkRequest onError(NetworkErrorCallback cb) &&;
    NetworkRequest onSuccess(NetworkSuccessCallback cb) &&;
    NetworkRequest finally(NetworkFinallyCallback cb) &&;

    /// Hide the request body in logs
    NetworkRequest hideRequestBody() &&;

    NetworkRequest payload(const QByteArray &payload) &&;
    NetworkRequest cache() &&;

    NetworkRequest caller(const QObject *caller) &&;
    NetworkRequest header(const char *headerName, const char *value) &&;
    NetworkRequest header(const char *headerName, const QByteArray &value) &&;
    NetworkRequest header(const char *headerName, const QString &value) &&;
    NetworkRequest header(const QByteArray &headerName,
                          const QByteArray &value) &&;
    NetworkRequest header(QNetworkRequest::KnownHeaders header,
                          const QVariant &value) &&;
    NetworkRequest attribute(QNetworkRequest::Attribute attribute,
                             const QVariant &value) &&;
    NetworkRequest headerList(
        const std::vector<std::pair<QByteArray, QByteArray>> &headers) &&;
    NetworkRequest timeout(int ms) &&;
    NetworkRequest concurrent() &&;
    NetworkRequest multiPart(QHttpMultiPart *payload) &&;

    NetworkRequest followRedirects(bool on) &&;
    NetworkRequest json(const QJsonObject &root) &&;
    NetworkRequest json(const QJsonArray &root) &&;
    NetworkRequest json(const QJsonDocument &document) &&;
    NetworkRequest json(const QByteArray &payload) &&;

#ifndef NDEBUG
    NetworkRequest ignoreSslErrors(bool ignore) &&;
#endif

    void execute();

private:
    void initializeDefaultValues();
};

}  // namespace chatterino

// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QMimeData>
#include <QMutex>
#include <QString>
#include <QStringView>

#include <memory>
#include <queue>

namespace chatterino {

class ResizingTextEdit;
class Channel;
class NetworkResult;
using ChannelPtr = std::shared_ptr<Channel>;

}  // namespace chatterino

namespace chatterino::imageuploader::detail {

QString getJSONValue(QJsonValue responseJson, QStringView jsonPattern);

QString getLinkFromResponse(const NetworkResult &response, QString pattern);

}  // namespace chatterino::imageuploader::detail

namespace chatterino {

struct RawImageData {
    QByteArray data;
    QString format;
    QString filePath;
    QString mimeType;
};

class ImageUploader final
{
public:
    std::pair<std::queue<RawImageData>, QString> getImages(
        const QMimeData *source) const;

    void upload(std::queue<RawImageData> images, ChannelPtr channel,
                QPointer<ResizingTextEdit> outputTextEdit);

private:
    void sendImageUploadRequest(RawImageData imageData, ChannelPtr channel,
                                QPointer<ResizingTextEdit> textEdit);

    void handleSuccessfulUpload(const NetworkResult &result,
                                QString originalFilePath, ChannelPtr channel,
                                QPointer<ResizingTextEdit> textEdit);
    void handleFailedUpload(const NetworkResult &result, ChannelPtr channel);

    void logToFile(const QString &originalFilePath, const QString &imageLink,
                   const QString &deletionLink, ChannelPtr channel);

    QMutex uploadMutex_;
    std::queue<RawImageData> uploadQueue_;
};
}  // namespace chatterino

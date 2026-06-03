#pragma once

#include <QString>

#include <vector>

namespace chatterino {

struct PubSubListenMessage {
    const std::vector<QString> topics;

    const QString nonce;

    QString token;

    PubSubListenMessage(std::vector<QString> _topics);

    void setToken(const QString &_token);

    QByteArray toJson() const;
};

}

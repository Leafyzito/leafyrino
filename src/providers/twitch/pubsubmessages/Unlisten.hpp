#pragma once

#include <QString>

#include <vector>

namespace chatterino {

struct PubSubUnlistenMessage {
    const std::vector<QString> topics;

    const QString nonce;

    PubSubUnlistenMessage(std::vector<QString> _topics);

    QByteArray toJson() const;
};

}

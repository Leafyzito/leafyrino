#include "providers/twitch/pubsubmessages/Listen.hpp"

#include "util/Helpers.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace chatterino {

namespace {

    QString normalizePubSubAuthToken(QString token)
    {
        token = token.trimmed();

        while (!token.isEmpty())
        {
            const auto previous = token;

            if (token.size() >= 2 &&
                ((token.startsWith('"') && token.endsWith('"')) ||
                 (token.startsWith('\'') && token.endsWith('\''))))
            {
                token = token.mid(1, token.size() - 2).trimmed();
            }

            if (token.startsWith("Authorization:", Qt::CaseInsensitive))
            {
                token = token.mid(QString("Authorization:").size()).trimmed();
            }

            if (token.startsWith("OAuth ", Qt::CaseInsensitive))
            {
                token = token.mid(QString("OAuth ").size()).trimmed();
            }

            if (token.startsWith("Bearer ", Qt::CaseInsensitive))
            {
                token = token.mid(QString("Bearer ").size()).trimmed();
            }

            if (token.startsWith("oauth:", Qt::CaseInsensitive))
            {
                token = token.mid(QString("oauth:").size()).trimmed();
            }

            if (token == previous)
            {
                break;
            }
        }

        return token;
    }

}

PubSubListenMessage::PubSubListenMessage(std::vector<QString> _topics)
    : topics(std::move(_topics))
    , nonce(generateUuid())
{
}

void PubSubListenMessage::setToken(const QString &_token)
{
    this->token = normalizePubSubAuthToken(_token);
}

QByteArray PubSubListenMessage::toJson() const
{
    QJsonObject root;

    root["type"] = "LISTEN";
    root["nonce"] = this->nonce;

    {
        QJsonObject data;

        QJsonArray jsonTopics;

        std::copy(this->topics.begin(), this->topics.end(),
                  std::back_inserter(jsonTopics));

        data["topics"] = jsonTopics;

        if (!this->token.isEmpty())
        {
            data["auth_token"] = this->token;
        }

        root["data"] = data;
    }

    return QJsonDocument(root).toJson();
}

}

// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/PubSubManager.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "providers/liveupdates/BasicPubSubManager.hpp"
#include "providers/twitch/PubSubClient.hpp"

#include <QJsonArray>
#include <QStringList>

#include <memory>
#include <utility>

using namespace std::chrono_literals;

namespace chatterino {

namespace {

QString authenticatedTopicUserID(const QString &topic)
{
    const QStringList prefixes{
        QStringLiteral("predictions-user-v1."),
        QStringLiteral("community-points-user-v1."),
        QStringLiteral("chatrooms-user-v1."),
    };

    for (const auto &prefix : prefixes)
    {
        if (topic.startsWith(prefix))
        {
            return topic.mid(prefix.size());
        }
    }

    return {};
}

}  // namespace

class PubSubManagerPrivate
    : public BasicPubSubManager<PubSubManagerPrivate, PubSubClient>
{
public:
    PubSubManagerPrivate(PubSub &parent, QString host,
                         std::chrono::milliseconds heartbeatInterval);
    ~PubSubManagerPrivate() override;
    PubSubManagerPrivate(const PubSubManagerPrivate &) = delete;
    PubSubManagerPrivate(PubSubManagerPrivate &&) = delete;
    PubSubManagerPrivate &operator=(const PubSubManagerPrivate &) = delete;
    PubSubManagerPrivate &operator=(PubSubManagerPrivate &&) = delete;

    std::shared_ptr<PubSubClient> makeClient();
    void checkHeartbeats();

    void reconnect()
    {
        for (const auto &[id, c] : this->clients())
        {
            c->close();
        }
    }

    std::chrono::milliseconds heartbeatInterval;
    QTimer heartbeatTimer;

    PubSub &parent;

    friend BasicPubSubManager<PubSubManagerPrivate, PubSubClient>;
    friend PubSub;
};

PubSubManagerPrivate::PubSubManagerPrivate(
    PubSub &parent, QString host, std::chrono::milliseconds heartbeatInterval)
    : BasicPubSubManager(std::move(host), "PubSub")
    , heartbeatInterval(heartbeatInterval)
    , parent(parent)
{
    QObject::connect(&this->heartbeatTimer, &QTimer::timeout, this,
                     &PubSubManagerPrivate::checkHeartbeats);
    this->heartbeatTimer.setInterval(this->heartbeatInterval);
    this->heartbeatTimer.setSingleShot(false);
    this->heartbeatTimer.start();
}

PubSubManagerPrivate::~PubSubManagerPrivate()
{
    this->stop();
}

void PubSubManagerPrivate::checkHeartbeats()
{
    for (const auto &[id, client] : this->clients())
    {
        client->checkHeartbeat();
    }
}

std::shared_ptr<PubSubClient> PubSubManagerPrivate::makeClient()
{
    return std::make_shared<PubSubClient>(this->parent,
                                          this->heartbeatInterval);
}

PubSub::PubSub(const QString &host, std::chrono::seconds heartbeatInterval)
    : private_(new PubSubManagerPrivate(*this, host, heartbeatInterval))
{
}

PubSub::~PubSub() = default;

const liveupdates::Diag &PubSub::wsDiag() const
{
    return this->private_->diag;
}

void PubSub::stop()
{
    this->private_->stop();
}

void PubSub::listenToChannelPointRewards(const QString &channelID)
{
    static const QString topicFormat("community-points-channel-v1.%1");
    assert(!channelID.isEmpty());

    auto topic = topicFormat.arg(channelID);

    qCDebug(chatterinoPubSub) << "Listen to topic" << topic;
    this->private_->subscribe(TopicData{.topic = std::move(topic)});
}

void PubSub::listenToPinnedChatUpdates(const QString &channelID)
{
    static const QString topicFormat("pinned-chat-updates-v1.%1");
    assert(!channelID.isEmpty());

    auto topic = topicFormat.arg(channelID);

    qCDebug(chatterinoPubSub) << "Listen to topic" << topic;
    this->private_->subscribe(TopicData{.topic = std::move(topic)});
}

void PubSub::listenToPredictions(const QString &channelID)
{
    static const QString topicFormat("predictions-channel-v1.%1");
    assert(!channelID.isEmpty());

    auto topic = topicFormat.arg(channelID);

    qCDebug(chatterinoPubSub) << "Listen to topic" << topic;
    this->private_->subscribe(TopicData{.topic = std::move(topic)});
}

void PubSub::listenToPolls(const QString &channelID)
{
    static const QString topicFormat("polls.%1");
    assert(!channelID.isEmpty());

    auto topic = topicFormat.arg(channelID);

    qCDebug(chatterinoPubSub) << "Listen to topic" << topic;
    this->private_->subscribe(TopicData{.topic = std::move(topic)});
}

void PubSub::listenToRaids(const QString &channelID)
{
    static const QString topicFormat("raid.%1");
    assert(!channelID.isEmpty());

    auto topic = topicFormat.arg(channelID);

    qCDebug(chatterinoPubSub) << "Listen to topic" << topic;
    this->private_->subscribe(TopicData{.topic = std::move(topic)});
}

void PubSub::listenToUserPredictions(const QString &userID,
                                     const QString &authToken)
{
    static const QString topicFormat("predictions-user-v1.%1");
    assert(!userID.isEmpty());

    auto topic = topicFormat.arg(userID);

    qCDebug(chatterinoPubSub) << "Listen to topic" << topic;
    this->listenToAuthenticatedTopic(std::move(topic), authToken);
}

void PubSub::listenToUserChannelPoints(const QString &userID,
                                       const QString &authToken)
{
    static const QString topicFormat("community-points-user-v1.%1");
    assert(!userID.isEmpty());

    auto topic = topicFormat.arg(userID);

    qCDebug(chatterinoPubSub) << "Listen to topic" << topic;
    this->listenToAuthenticatedTopic(std::move(topic), authToken);
}

void PubSub::listenToChatWarnings(const QString &userID,
                                  const QString &authToken)
{
    static const QString topicFormat("chatrooms-user-v1.%1");
    assert(!userID.isEmpty());

    auto topic = topicFormat.arg(userID);

    qCDebug(chatterinoPubSub) << "Listen to topic" << topic;
    this->listenToAuthenticatedTopic(std::move(topic), authToken);
}

void PubSub::forgetUserAuthenticatedTopics(const QString &userID)
{
    if (userID.isEmpty())
    {
        return;
    }

    const QStringList topics{
        QString("predictions-user-v1.%1").arg(userID),
        QString("community-points-user-v1.%1").arg(userID),
        QString("chatrooms-user-v1.%1").arg(userID),
    };

    for (const auto &topic : topics)
    {
        auto it = this->authenticatedTopicTokens_.find(topic);
        if (it == this->authenticatedTopicTokens_.end())
        {
            continue;
        }

        this->private_->unsubscribe(TopicData{.topic = topic});
        this->authenticatedTopicTokens_.erase(it);
    }
}

void PubSub::forgetOtherUserAuthenticatedTopics(const QString &activeUserID)
{
    const auto expectedUserID = activeUserID.trimmed();
    for (auto it = this->authenticatedTopicTokens_.begin();
         it != this->authenticatedTopicTokens_.end();)
    {
        const auto topicUserID = authenticatedTopicUserID(it->first);
        if (topicUserID.isEmpty() || topicUserID == expectedUserID)
        {
            ++it;
            continue;
        }

        this->private_->unsubscribe(TopicData{.topic = it->first});
        it = this->authenticatedTopicTokens_.erase(it);
    }
}

void PubSub::listenToAuthenticatedTopic(QString topic, const QString &authToken)
{
    const auto normalizedToken = authToken.trimmed();
    if (topic.isEmpty() || normalizedToken.isEmpty())
    {
        return;
    }

    auto it = this->authenticatedTopicTokens_.find(topic);
    if (it != this->authenticatedTopicTokens_.end())
    {
        if (it->second == normalizedToken)
        {
            return;
        }

        this->private_->unsubscribe(TopicData{.topic = topic});
        it->second = normalizedToken;
    }
    else
    {
        this->authenticatedTopicTokens_.emplace(topic, normalizedToken);
    }

    this->private_->subscribe(
        TopicData{.topic = std::move(topic), .authToken = normalizedToken});
}

void PubSub::reconnect()
{
    this->private_->reconnect();
}

}  // namespace chatterino

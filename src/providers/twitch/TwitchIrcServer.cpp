// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/TwitchIrcServer.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/Common.hpp"
#include "common/Env.hpp"
#include "common/Literals.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/bttv/BttvLiveUpdates.hpp"
#include "providers/bttv/liveupdates/BttvLiveUpdateMessages.hpp"  // IWYU pragma: keep
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/irc/IrcConnection2.hpp"
#include "providers/moltorino/MoltorinoSupporterBadges.hpp"
#include "providers/seventv/eventapi/Dispatch.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvEventAPI.hpp"
#include "providers/seventv/SeventvPersonalEmotes.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/IrcMessageHandler.hpp"
#include "providers/twitch/PubSubManager.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "providers/twitch/TwitchHelpers.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/PostToThread.hpp"
#include "util/RatelimitBucket.hpp"
#include "util/Twitch.hpp"

#include <IrcCommand>
#include <IrcMessage>
#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QCoreApplication>
#include <QMetaEnum>
#include <QRandomGenerator>
#include <QTimer>

#include <algorithm>
#include <cassert>
#include <functional>
#include <mutex>
#include <ranges>

using namespace std::chrono_literals;

namespace {

using namespace chatterino;

// Ratelimits for anonymousJoinBucket_
constexpr int JOIN_RATELIMIT_BUDGET = 18;
constexpr int JOIN_RATELIMIT_COOLDOWN = 12500;

bool isWarningAcknowledgeNotice(const QString &text)
{
    return text.startsWith(
               "You received a Warning from a moderator in this channel.",
               Qt::CaseInsensitive) ||
           text.contains("Acknowledge the Warning at", Qt::CaseInsensitive);
}

QString makeWebClientNonce()
{
    QString nonce;
    nonce.reserve(32);

    auto *random = QRandomGenerator::global();
    for (int i = 0; i < 4; ++i)
    {
        nonce += QStringLiteral("%1").arg(random->generate(), 8, 16,
                                          QLatin1Char('0'));
    }

    return nonce.toLower();
}

thread_local bool preferAnonymousTwitchChannels = false;

class ScopedAnonymousTwitchLookup
{
public:
    explicit ScopedAnonymousTwitchLookup(bool enabled)
        : previous_(preferAnonymousTwitchChannels)
    {
        preferAnonymousTwitchChannels = enabled;
    }

    ~ScopedAnonymousTwitchLookup()
    {
        preferAnonymousTwitchChannels = this->previous_;
    }

private:
    bool previous_;
};

QStringList makeIrcTags(QStringList tags = {})
{
    if (getSettings()->fakeWebChat)
    {
        tags.prepend(QStringLiteral("client-nonce=") + makeWebClientNonce());
    }

    return tags;
}

QString makePrivmsg(const QString &channelName, const QString &message,
                    QStringList tags = {})
{
    QString prefix;
    if (!tags.isEmpty())
    {
        prefix = QStringLiteral("@") + tags.join(QLatin1Char(';')) +
                 QStringLiteral(" ");
    }

    return prefix + QStringLiteral("PRIVMSG #") + channelName +
           QStringLiteral(" :") + message;
}

void sendHelixMessage(const std::shared_ptr<TwitchChannel> &channel,
                      const QString &message, const QString &replyParentId = {})
{
    auto broadcasterID = channel->roomId();
    if (broadcasterID.isEmpty())
    {
        channel->addSystemMessage(
            "Sending messages in this channel isn't possible.");
        return;
    }

    getHelix()->sendChatMessage(
        {
            .broadcasterID = broadcasterID,
            .senderID =
                getApp()->getAccounts()->twitch.getCurrent()->getUserId(),
            .message = message,
            .replyParentMessageID = replyParentId,
        },
        [weak = std::weak_ptr(channel)](const auto &res) {
            auto chan = weak.lock();
            if (!chan)
            {
                return;
            }

            if (res.isSent)
            {
                return;
            }

            if (res.dropReason)
            {
                if (isWarningAcknowledgeNotice(res.dropReason->message))
                {
                    chan->handleChatWarningNotice();
                    return;
                }

                chan->addSystemMessage(res.dropReason->message);
            }
            else
            {
                chan->addSystemMessage("Your message was not sent.");
            }
        },
        [weak = std::weak_ptr(channel)](auto error, auto message) {
            auto chan = weak.lock();
            if (!chan)
            {
                return;
            }

            if (message.isEmpty())
            {
                message = "(empty message)";
            }

            using Error = decltype(error);

            auto errorMessage = [&]() -> QString {
                switch (error)
                {
                    case Error::MissingText:
                        return "You can't send an empty message.";
                    case Error::BadRequest:
                        return "Failed to send message: " + message;
                    case Error::Forbidden:
                        return "You are not allowed to send messages in this "
                               "channel.";
                    case Error::MessageTooLarge:
                        return "Your message was too long.";
                    case Error::UserMissingScope:
                        return "Missing required scope. Re-login with your "
                               "account and try again.";
                    case Error::Forwarded:
                        return message;
                    case Error::Unknown:
                    default:
                        return "Unknown error: " + message;
                }
            }();
            if (isWarningAcknowledgeNotice(errorMessage))
            {
                chan->handleChatWarningNotice();
                return;
            }

            chan->addSystemMessage(errorMessage);
        });
}

}  // namespace

namespace chatterino {

using namespace literals;

TwitchIrcServer::TwitchIrcServer()
    : whispersChannel(new Channel("/whispers", Channel::Type::TwitchWhispers))
    , mentionsChannel(new Channel("/mentions", Channel::Type::TwitchMentions))
    , liveChannel(new Channel("/live", Channel::Type::TwitchLive))
    , automodChannel(new Channel("/automod", Channel::Type::TwitchAutomod))
    , watchingChannel(Channel::getEmpty(), Channel::Type::TwitchWatching)
    , readConnection_(
          createTwitchConnectionPool(
              this, getSettings()->twitchReadConnectionMode ==
                        TwitchReadConnectionMode::AnonymousParallel),
          {})
{
    // Initialize the connections
    // XXX: don't create write connection if there is no separate write connection.
    this->writeConnection_.reset(new IrcConnection);
    this->writeConnection_->moveToThread(
        QCoreApplication::instance()->thread());

    QObject::connect(this->writeConnection_.get(),
                     &Communi::IrcConnection::messageReceived, this,
                     &TwitchIrcServer::writeConnectionMessageReceived);
    this->signalHolder.managedConnect(
        this->writeConnection_->connectionLost, [this](bool timeout) {
            qCDebug(chatterinoIrc)
                << "Write connection reconnect requested. Timeout:" << timeout;
            this->writeConnection_->smartReconnect();
        });
    this->signalHolder.managedConnect(
        this->readConnection_->messageReceived, [this](auto *msg) {
            this->readConnectionMessageReceived(msg);
        });
    this->signalHolder.managedConnect(
        this->readConnection_->privateMessageReceived, [this](auto *msg) {
            this->privateMessageReceived(msg);
        });

    auto actuallyJoinAnonymous = [this](QString message) {
        if (!this->anonymousChannels.contains(message) ||
            !this->anonymousReadConnection_)
        {
            return;
        }
        this->anonymousReadConnection_->sendRaw("JOIN #" + message);
    };
    this->anonymousJoinBucket_.reset(
        new RatelimitBucket(JOIN_RATELIMIT_BUDGET, JOIN_RATELIMIT_COOLDOWN,
                            actuallyJoinAnonymous, this));

    this->anonymousReadConnection_.reset(new IrcConnection);
    this->anonymousReadConnection_->moveToThread(
        QCoreApplication::instance()->thread());

    QObject::connect(this->anonymousReadConnection_.get(),
                     &Communi::IrcConnection::messageReceived, this,
                     [this](auto *msg) {
                         if (msg->type() == Communi::IrcMessage::Type::Private)
                         {
                             return;
                         }
                         if (msg->command() == "RECONNECT")
                         {
                             this->markAnonymousChannelsConnected();
                             this->reconnectAnonymousChannels();
                             return;
                         }
                         ScopedAnonymousTwitchLookup lookup(true);
                         this->readConnectionMessageReceived(msg);
                     });
    QObject::connect(this->anonymousReadConnection_.get(),
                     &Communi::IrcConnection::privateMessageReceived, this,
                     [this](auto *msg) {
                         ScopedAnonymousTwitchLookup lookup(true);
                         this->privateMessageReceived(msg);
                     });
    QObject::connect(this->anonymousReadConnection_.get(),
                     &Communi::IrcConnection::connected, this, [this] {
                         this->onAnonymousReadConnected(
                             this->anonymousReadConnection_.get());
                     });
    QObject::connect(this->anonymousReadConnection_.get(),
                     &Communi::IrcConnection::disconnected, this, [this] {
                         this->onAnonymousDisconnected();
                         this->anonymousReadConnectionStarted_ = false;
                     });
    this->signalHolder.managedConnect(
        this->anonymousReadConnection_->connectionLost, [this](bool timeout) {
            qCDebug(chatterinoIrc)
                << "Anonymous read connection reconnect requested. Timeout:"
                << timeout;
            this->anonymousReadConnection_->smartReconnect();
        });
    this->signalHolder.managedConnect(
        this->anonymousReadConnection_->heartbeat, [this] {
            this->markAnonymousChannelsConnected();
        });
}

void TwitchIrcServer::initialize()
{
    getApp()->getAccounts()->twitch.currentUserChanged.connect([this]() {
        postToThread([this] {
            if (getSettings()->twitchReadConnectionMode ==
                TwitchReadConnectionMode::Authenticated)
            {
                this->connect();
            }
            else
            {
                this->writeConnection_->close();
                TwitchIrcServer::initializeConnection(
                    this->writeConnection_.get(), ConnectionType::Write);
            }
        });
    });

    this->signalHolder.managedConnect(
        getApp()->getTwitchPubSub()->pointReward.redeemed, [this](auto &data) {
            QString channelId = data.value("channel_id").toString();
            if (channelId.isEmpty())
            {
                qCDebug(chatterinoApp)
                    << "Couldn't find channel id of point reward";
                return;
            }

            auto chan = this->getChannelOrEmptyByID(channelId);

            auto reward = ChannelPointReward(data);

            postToThread([chan, reward] {
                if (isAppAboutToQuit())
                {
                    return;
                }

                if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
                {
                    channel->addChannelPointReward(reward);
                }
            });
        });

    this->signalHolder.managedConnect(
        getApp()->getTwitchPubSub()->pinnedChat.updated, [this](auto &data) {
            if (!getSettings()->enablePinnedMessages)
            {
                return;
            }
            QString topic = data.value("topic").toString();

            if (!topic.startsWith("pinned-chat-updates-v1."))
            {
                return;
            }
            QString channelId = topic.mid(23);
            if (channelId.isEmpty())
            {
                return;
            }

            auto chan = this->getChannelOrEmptyByID(channelId);

            postToThread([chan, data] {
                if (isAppAboutToQuit())
                {
                    return;
                }

                if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
                {
                    channel->handlePinnedChatUpdate(data);
                }
            });
        });

    this->signalHolder.managedConnect(
        getApp()->getTwitchPubSub()->prediction.updated,
        [this](const auto &data) {
            QString topic = data.value("topic").toString();

            if (!topic.startsWith("predictions-channel-v1."))
            {
                return;
            }
            QString channelId = topic.mid(23);
            if (channelId.isEmpty())
            {
                return;
            }

            auto chan = this->getChannelOrEmptyByID(channelId);

            postToThread([chan, data] {
                if (isAppAboutToQuit())
                {
                    return;
                }

                if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
                {
                    channel->handlePredictionUpdate(data);
                }
            });
        });

    this->signalHolder.managedConnect(
        getApp()->getTwitchPubSub()->chatWarning.updated,
        [this](const auto &payload) {
            const auto data = payload.value("data").toObject();
            const auto channelId = data.value("channel_id").toString();
            if (channelId.isEmpty())
            {
                qCDebug(chatterinoApp)
                    << "Couldn't find channel id of chat warning";
                return;
            }

            auto chan = this->getChannelOrEmptyByID(channelId);

            postToThread([chan, payload] {
                if (isAppAboutToQuit())
                {
                    return;
                }

                if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
                {
                    channel->handleChatWarningPubSub(payload);
                }
            });
        });

    getSettings()->enableBTTVChannelEmotes.connect(
        [this] {
            this->reloadAllBTTVChannelEmotes();
        },
        this->signalHolder, false);
    getSettings()->enableFFZChannelEmotes.connect(
        [this] {
            this->reloadAllFFZChannelEmotes();
        },
        this->signalHolder, false);
    getSettings()->enableSevenTVChannelEmotes.connect(
        [this] {
            this->reloadAllSevenTVChannelEmotes();
        },
        this->signalHolder, false);
}

void TwitchIrcServer::aboutToQuit()
{
    this->signalHolder.clear();

    this->channels.clear();
    this->anonymousChannels.clear();
}

void TwitchIrcServer::initializeConnection(IrcConnection *connection,
                                           ConnectionType type)
{
    std::shared_ptr<TwitchAccount> account =
        getApp()->getAccounts()->twitch.getCurrent();

    // twitch.tv/tags enables IRCv3 tags on messages. See https://dev.twitch.tv/docs/irc/tags
    // twitch.tv/commands enables a bunch of miscellaneous command capabilities. See https://dev.twitch.tv/docs/irc/commands
    // twitch.tv/membership enables the JOIN/PART/NAMES commands. See https://dev.twitch.tv/docs/irc/membership
    // This is enabled so we receive USERSTATE messages when joining channels / typing messages, along with the other command capabilities
    QStringList caps{"twitch.tv/tags", "twitch.tv/commands"};
    if (type != ConnectionType::Write)
    {
        caps.push_back("twitch.tv/membership");
    }

    connection->network()->setSkipCapabilityValidation(true);
    connection->network()->setRequestedCapabilities(caps);

    QString username = account->getUserName();
    QString oauthToken = account->getOAuthToken();

    if (!oauthToken.startsWith("oauth:"))
    {
        oauthToken.prepend("oauth:");
    }

    bool anon = account->isAnon();
    if (type == ConnectionType::AnonymousRead)
    {
        username =
            QStringLiteral("justinfan%1")
                .arg(QRandomGenerator::global()->bounded(100000, 1000000));
        oauthToken = {};
        anon = true;
    }
    else if (type == ConnectionType::Read &&
             getSettings()->twitchReadConnectionMode !=
                 TwitchReadConnectionMode::Authenticated)
    {
        username = ANONYMOUS_USERNAME;
        oauthToken = {};
        anon = true;
    }

    qCDebug(chatterinoIrc).noquote()
        << "Initializing a" << qmagicenum::enumName(type) << "connection {"
        << "username:" << username << "anon:" << anon << "}";

    connection->setUserName(username);
    connection->setNickName(username);
    connection->setRealName(username);

    if (!anon)
    {
        connection->setPassword(oauthToken);
    }

    // https://dev.twitch.tv/docs/irc#connecting-to-the-twitch-irc-server
    // SSL disabled: irc://irc.chat.twitch.tv:6667 (or port 80)
    // SSL enabled: irc://irc.chat.twitch.tv:6697 (or port 443)
    connection->setHost(Env::get().twitchServerHost);
    connection->setPort(Env::get().twitchServerPort);
    connection->setSecure(Env::get().twitchServerSecure);

    connection->open();
}

std::shared_ptr<Channel> TwitchIrcServer::createChannel(
    const QString &channelName, bool anonymous)
{
    auto channel = std::make_shared<TwitchChannel>(channelName, anonymous);
    channel->initialize();

    // We can safely ignore these signal connections since the TwitchIrcServer is only
    // ever destroyed when the full Application state is about to be destroyed, at which point
    // no Channel's should live
    // NOTE: CHANNEL_LIFETIME
    std::ignore = channel->sendMessageSignal.connect(
        [this, channel = std::weak_ptr(channel)](auto &msg, bool &sent) {
            auto c = channel.lock();
            if (!c)
            {
                return;
            }
            this->onMessageSendRequested(c, msg, sent);
        });
    std::ignore = channel->sendReplySignal.connect(
        [this, channel = std::weak_ptr(channel)](auto &msg, auto &replyId,
                                                 bool &sent) {
            auto c = channel.lock();
            if (!c)
            {
                return;
            }
            this->onReplySendRequested(c, msg, replyId, sent);
        });

    return channel;
}

void TwitchIrcServer::privateMessageReceived(
    Communi::IrcPrivateMessage *message)
{
    if (preferAnonymousTwitchChannels)
    {
        QString channelName;
        if (!trimChannelName(message->target(), channelName))
        {
            return;
        }

        auto chan = this->getAnonymousChannelOrEmpty(channelName);
        auto *twitchChannel = dynamic_cast<TwitchChannel *>(chan.get());
        if (!twitchChannel)
        {
            return;
        }

        IrcMessageHandler::parsePrivMessageInto(message, *twitchChannel,
                                                twitchChannel);
        return;
    }

    IrcMessageHandler::instance().handlePrivMessage(message, *this);
}

void TwitchIrcServer::readConnectionMessageReceived(
    Communi::IrcMessage *message)
{
    const QString &command = message->command();

    auto &handler = IrcMessageHandler::instance();

    // Below commands enabled through the twitch.tv/membership CAP REQ
    if (command == "JOIN")
    {
        handler.handleJoinMessage(message);
    }
    else if (command == "PART")
    {
        handler.handlePartMessage(message);
    }
    else if (command == "USERSTATE")
    {
        // Received USERSTATE upon JOINing a channel
        handler.handleUserStateMessage(message);
    }
    else if (command == "ROOMSTATE")
    {
        // Received ROOMSTATE upon JOINing a channel
        handler.handleRoomStateMessage(message);
    }
    else if (command == "CLEARCHAT")
    {
        handler.handleClearChatMessage(message);
    }
    else if (command == "CLEARMSG")
    {
        handler.handleClearMessageMessage(message);
    }
    else if (command == "USERNOTICE")
    {
        handler.handleUserNoticeMessage(message, *this);
    }
    else if (command == "NOTICE")
    {
        handler.handleNoticeMessage(
            static_cast<Communi::IrcNoticeMessage *>(message));
    }
}

void TwitchIrcServer::writeConnectionMessageReceived(
    Communi::IrcMessage *message)
{
    const QString &command = message->command();

    auto &handler = IrcMessageHandler::instance();
    // Below commands enabled through the twitch.tv/commands CAP REQ
    if (command == "USERSTATE")
    {
        // Received USERSTATE upon sending PRIVMSG messages
        handler.handleUserStateMessage(message);
    }
    else if (command == "NOTICE")
    {
        // List of expected NOTICE messages on write connection
        // https://git.kotmisia.pl/Mm2PL/docs/src/branch/master/irc_msg_ids.md#command-results
        handler.handleNoticeMessage(
            static_cast<Communi::IrcNoticeMessage *>(message));
    }
    else if (command == "RECONNECT")
    {
        this->addGlobalSystemMessage(
            "Twitch Servers requested us to reconnect, reconnecting");
        this->connect();
    }
    else if (command == "WHISPER")
    {
        handler.handleWhisperMessage(message);
    }
}

void TwitchIrcServer::onAnonymousReadConnected(IrcConnection *connection)
{
    (void)connection;

    std::vector<ChannelPtr> activeChannels;
    {
        std::lock_guard lock(this->channelMutex);

        activeChannels.reserve(this->anonymousChannels.size());
        for (const auto &weak : this->anonymousChannels)
        {
            if (auto channel = weak.lock())
            {
                activeChannels.push_back(channel);
            }
        }
    }

    auto visible = getApp()->getWindows()->getVisibleChannelNames();

    std::ranges::stable_partition(activeChannels, [&](const auto &chan) {
        return visible.contains(chan->getName());
    });

    for (const auto &channel : activeChannels)
    {
        if (channel->getName().startsWith("/"))
        {
            continue;
        }
        this->anonymousJoinBucket_->send(channel->getName());
    }

    auto connectedMsg = makeSystemMessage("connected anonymously");
    connectedMsg->flags.set(MessageFlag::ConnectedMessage);
    auto reconnected = makeSystemMessage("reconnected anonymously");
    reconnected->flags.set(MessageFlag::ConnectedMessage);

    for (const auto &chan : activeChannels)
    {
        MessagePtr last = chan->getLastMessage();

        bool replaceMessage =
            last && last->flags.has(MessageFlag::DisconnectedMessage);

        if (replaceMessage)
        {
            chan->replaceMessage(last, reconnected);
        }
        else
        {
            chan->addMessage(connectedMsg, MessageContext::Original);
        }
    }
}

void TwitchIrcServer::onAnonymousDisconnected()
{
    std::lock_guard<std::mutex> lock(this->channelMutex);

    MessageBuilder b(systemMessage, "anonymous disconnected");
    b->flags.set(MessageFlag::DisconnectedMessage);
    auto disconnectedMsg = b.release();

    for (std::weak_ptr<Channel> &weak : this->anonymousChannels.values())
    {
        std::shared_ptr<Channel> chan = weak.lock();
        if (!chan)
        {
            continue;
        }

        chan->addMessage(disconnectedMsg, MessageContext::Original);

        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
        {
            channel->markDisconnected();
        }
    }
}

void TwitchIrcServer::markAnonymousChannelsConnected()
{
    std::lock_guard<std::mutex> lock(this->channelMutex);

    for (std::weak_ptr<Channel> &weak : this->anonymousChannels.values())
    {
        auto chan = weak.lock();
        if (!chan)
        {
            continue;
        }

        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
        {
            channel->markConnected();
        }
    }
}

void TwitchIrcServer::ensureAnonymousReadConnection()
{
    bool shouldStart = false;
    {
        std::lock_guard<std::mutex> locker(this->connectionMutex_);

        if (this->anonymousReadConnection_ &&
            !this->anonymousReadConnection_->isConnected() &&
            !this->anonymousReadConnectionStarted_)
        {
            this->anonymousReadConnectionStarted_ = true;
            shouldStart = true;
        }
    }

    if (!shouldStart)
    {
        return;
    }

    TwitchIrcServer::initializeConnection(this->anonymousReadConnection_.get(),
                                          ConnectionType::AnonymousRead);
}

std::shared_ptr<Channel> TwitchIrcServer::getCustomChannel(
    const QString &channelName)
{
    if (channelName == "/whispers")
    {
        return this->whispersChannel;
    }

    if (channelName == "/mentions")
    {
        return this->mentionsChannel;
    }

    if (channelName == "/live")
    {
        return this->liveChannel;
    }

    if (channelName == "/automod")
    {
        return this->automodChannel;
    }

    static auto getTimer = [this](ChannelPtr channel, int msBetweenMessages,
                                  bool addInitialMessages) {
        if (addInitialMessages)
        {
            for (auto i = 0; i < 1000; i++)
            {
                channel->addSystemMessage(QString::number(i + 1));
            }
        }

        auto *timer = new QTimer;
        QObject::connect(timer, &QTimer::timeout, this, [channel] {
            channel->addSystemMessage(QTime::currentTime().toString());
        });
        timer->start(msBetweenMessages);
        return timer;
    };

    if (channelName == "$$$")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 500, true);

        return channel;
    }
    if (channelName == "$$$:e")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 500, false);

        return channel;
    }
    if (channelName == "$$$$")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 250, true);

        return channel;
    }
    if (channelName == "$$$$:e")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 250, false);

        return channel;
    }
    if (channelName == "$$$$$")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 100, true);

        return channel;
    }
    if (channelName == "$$$$$:e")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 100, false);

        return channel;
    }
    if (channelName == "$$$$$$")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 50, true);

        return channel;
    }
    if (channelName == "$$$$$$:e")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 50, false);

        return channel;
    }
    if (channelName == "$$$$$$$")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 25, true);

        return channel;
    }
    if (channelName == "$$$$$$$:e")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 25, false);

        return channel;
    }

    return nullptr;
}

void TwitchIrcServer::forEachChannelAndSpecialChannels(
    std::function<void(ChannelPtr)> func)
{
    this->forEachChannel(func);

    func(this->whispersChannel);
    func(this->mentionsChannel);
    func(this->liveChannel);
    func(this->automodChannel);
}

std::shared_ptr<Channel> TwitchIrcServer::getChannelOrEmptyByID(
    const QString &channelId)
{
    std::lock_guard<std::mutex> lock(this->channelMutex);

    for (const auto &weakChannel : this->channels)
    {
        auto channel = weakChannel.lock();
        if (!channel)
        {
            continue;
        }

        auto twitchChannel = std::dynamic_pointer_cast<TwitchChannel>(channel);
        if (!twitchChannel)
        {
            continue;
        }

        if (twitchChannel->roomId() == channelId &&
            twitchChannel->getName().count(':') < 2)
        {
            return twitchChannel;
        }
    }

    return Channel::getEmpty();
}

bool TwitchIrcServer::prepareToSend(
    const std::shared_ptr<TwitchChannel> &channel)
{
    std::lock_guard<std::mutex> guard(this->lastMessageMutex_);

    auto &lastMessage = channel->hasHighRateLimit() ? this->lastMessageMod_
                                                    : this->lastMessagePleb_;
    size_t maxMessageCount = channel->hasHighRateLimit() ? 99 : 19;
    if (getSettings()->useBotLimitsMessage)
    {
        maxMessageCount = 7499;
    }
    auto minMessageOffset = (channel->hasHighRateLimit() ? 100ms : 1100ms);

    auto now = std::chrono::steady_clock::now();

    // check if you are sending messages too fast
    if (!lastMessage.empty() && lastMessage.back() + minMessageOffset > now)
    {
        if (this->lastErrorTimeSpeed_ + 30s < now)
        {
            channel->addSystemMessage("You are sending messages too quickly.");

            this->lastErrorTimeSpeed_ = now;
        }
        return false;
    }

    // remove messages older than 30 seconds
    while (!lastMessage.empty() && lastMessage.front() + 32s < now)
    {
        lastMessage.pop();
    }

    // check if you are sending too many messages
    if (lastMessage.size() >= maxMessageCount)
    {
        if (this->lastErrorTimeAmount_ + 30s < now)
        {
            channel->addSystemMessage("You are sending too many messages.");

            this->lastErrorTimeAmount_ = now;
        }
        return false;
    }

    lastMessage.push(now);
    return true;
}

void TwitchIrcServer::onMessageSendRequested(
    const std::shared_ptr<TwitchChannel> &channel, const QString &message,
    bool &sent)
{
    sent = false;

    bool canSend = this->prepareToSend(channel);
    if (!canSend)
    {
        return;
    }

    if (getSettings()->shouldSendHelixChat())
    {
        sendHelixMessage(channel, message);
    }
    else
    {
        this->sendMessage(channel->getName(), message);
    }

    sent = true;
}

void TwitchIrcServer::onReplySendRequested(
    const std::shared_ptr<TwitchChannel> &channel, const QString &message,
    const QString &replyId, bool &sent)
{
    sent = false;

    bool canSend = this->prepareToSend(channel);
    if (!canSend)
    {
        return;
    }

    if (getSettings()->shouldSendHelixChat())
    {
        sendHelixMessage(channel, message, replyId);
    }
    else
    {
        this->sendRawMessage(makePrivmsg(
            channel->getName(), message,
            makeIrcTags(QStringList{QStringLiteral("reply-parent-msg-id=") +
                                    replyId})));
    }
    sent = true;
}

const IndirectChannel &TwitchIrcServer::getWatchingChannel() const
{
    return this->watchingChannel;
}

void TwitchIrcServer::setWatchingChannel(ChannelPtr newWatchingChannel)
{
    assertInGuiThread();

    this->watchingChannel.reset(newWatchingChannel);
}

ChannelPtr TwitchIrcServer::getWhispersChannel() const
{
    return this->whispersChannel;
}

ChannelPtr TwitchIrcServer::getMentionsChannel() const
{
    return this->mentionsChannel;
}

ChannelPtr TwitchIrcServer::getLiveChannel() const
{
    return this->liveChannel;
}

ChannelPtr TwitchIrcServer::getAutomodChannel() const
{
    return this->automodChannel;
}

QString TwitchIrcServer::getLastUserThatWhisperedMe() const
{
    return this->lastUserThatWhisperedMe.get();
}

void TwitchIrcServer::setLastUserThatWhisperedMe(const QString &user)
{
    assertInGuiThread();

    this->lastUserThatWhisperedMe.set(user);
}

void TwitchIrcServer::initEventAPIs(BttvLiveUpdates *bttvLiveUpdates,
                                    SeventvEventAPI *seventvEventAPI)
{
    assertInGuiThread();

    if (bttvLiveUpdates != nullptr)
    {
        this->signalHolder.managedConnect(
            bttvLiveUpdates->signals_.emoteAdded, [&](const auto &data) {
                auto chan = this->getChannelOrEmptyByID(data.channelID);

                postToThread(
                    [chan, data] {
                        if (auto *channel =
                                dynamic_cast<TwitchChannel *>(chan.get()))
                        {
                            channel->addBttvEmote(data);
                        }
                    },
                    this);
            });
        this->signalHolder.managedConnect(
            bttvLiveUpdates->signals_.emoteUpdated, [&](const auto &data) {
                auto chan = this->getChannelOrEmptyByID(data.channelID);

                postToThread(
                    [chan, data] {
                        if (auto *channel =
                                dynamic_cast<TwitchChannel *>(chan.get()))
                        {
                            channel->updateBttvEmote(data);
                        }
                    },
                    this);
            });
        this->signalHolder.managedConnect(
            bttvLiveUpdates->signals_.emoteRemoved, [&](const auto &data) {
                auto chan = this->getChannelOrEmptyByID(data.channelID);

                postToThread(
                    [chan, data] {
                        if (auto *channel =
                                dynamic_cast<TwitchChannel *>(chan.get()))
                        {
                            channel->removeBttvEmote(data);
                        }
                    },
                    this);
            });
    }
    else
    {
        qCDebug(chatterinoBttv)
            << "Skipping initialization of Live Updates as it's disabled";
    }

    if (seventvEventAPI != nullptr)
    {
        this->signalHolder.managedConnect(
            seventvEventAPI->signals_.emoteAdded, [&](const auto &data) {
                if (getApp()->getSeventvPersonalEmotes()->hasEmoteSet(
                        data.emoteSetID))
                {
                    getApp()->getSeventvPersonalEmotes()->updateEmoteSet(
                        data.emoteSetID, data);
                }
                else
                {
                    postToThread(
                        [this, data] {
                            this->forEachSeventvEmoteSet(
                                data.emoteSetID, [data](TwitchChannel &chan) {
                                    chan.addSeventvEmote(data);
                                });
                        },
                        this);
                }
            });
        this->signalHolder.managedConnect(
            seventvEventAPI->signals_.emoteUpdated, [&](const auto &data) {
                if (getApp()->getSeventvPersonalEmotes()->hasEmoteSet(
                        data.emoteSetID))
                {
                    getApp()->getSeventvPersonalEmotes()->updateEmoteSet(
                        data.emoteSetID, data);
                }
                else
                {
                    postToThread(
                        [this, data] {
                            this->forEachSeventvEmoteSet(
                                data.emoteSetID, [data](TwitchChannel &chan) {
                                    chan.updateSeventvEmote(data);
                                });
                        },
                        this);
                }
            });
        this->signalHolder.managedConnect(
            seventvEventAPI->signals_.emoteRemoved, [&](const auto &data) {
                if (getApp()->getSeventvPersonalEmotes()->hasEmoteSet(
                        data.emoteSetID))
                {
                    getApp()->getSeventvPersonalEmotes()->updateEmoteSet(
                        data.emoteSetID, data);
                }
                else
                {
                    postToThread(
                        [this, data] {
                            this->forEachSeventvEmoteSet(
                                data.emoteSetID, [data](TwitchChannel &chan) {
                                    chan.removeSeventvEmote(data);
                                });
                        },
                        this);
                }
            });
        this->signalHolder.managedConnect(
            seventvEventAPI->signals_.userUpdated, [&](const auto &data) {
                this->forEachSeventvUser(data.userID,
                                         [data](TwitchChannel &chan) {
                                             chan.updateSeventvUser(data);
                                         });
            });
        this->signalHolder.managedConnect(
            seventvEventAPI->signals_.personalEmoteSetAdded,
            [&](const seventv::eventapi::PersonalEmoteSetAdded &data) {
                QVarLengthArray<QString, 1> names;
                for (const auto &user : data.connections)
                {
                    if (const auto *u =
                            std::get_if<seventv::eventapi::TwitchUser>(&user))
                    {
                        names.emplace_back(u->userName);
                    }
                }
                if (names.empty())
                {
                    return;
                }

                postToThread(
                    [this, emoteSet = data.emoteSet,
                     names{std::move(names)}]() {
                        this->forEachChannelAndSpecialChannels([&](const auto
                                                                       &chan) {
                            if (auto *twitchChannel =
                                    dynamic_cast<TwitchChannel *>(chan.get()))
                            {
                                for (const auto &name : names)
                                {
                                    twitchChannel->upsertPersonalSeventvEmotes(
                                        name, emoteSet);
                                }
                            }
                        });
                    },
                    this);
            });
    }
    else
    {
        qCDebug(chatterinoSeventvEventAPI)
            << "Skipping initialization as the EventAPI is disabled";
    }
}

void TwitchIrcServer::reloadAllBTTVChannelEmotes()
{
    this->forEachChannel([](const auto &chan) {
        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
        {
            channel->refreshBTTVChannelEmotes(false);
        }
    });
}

void TwitchIrcServer::reloadAllFFZChannelEmotes()
{
    this->forEachChannel([](const auto &chan) {
        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
        {
            channel->refreshFFZChannelEmotes(false);
        }
    });
}

void TwitchIrcServer::reloadAllSevenTVChannelEmotes()
{
    this->forEachChannel([](const auto &chan) {
        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
        {
            channel->refreshSevenTVChannelEmotes(false);
        }
    });
}

void TwitchIrcServer::forEachSeventvEmoteSet(
    const QString &emoteSetId, std::function<void(TwitchChannel &)> func)
{
    this->forEachChannel([emoteSetId, func](const auto &chan) {
        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get());
            channel->seventvEmoteSetID() == emoteSetId)
        {
            func(*channel);
        }
    });
}
void TwitchIrcServer::forEachSeventvUser(
    const QString &userId, std::function<void(TwitchChannel &)> func)
{
    this->forEachChannel([userId, func](const auto &chan) {
        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get());
            channel->seventvUserID() == userId)
        {
            func(*channel);
        }
    });
}

void TwitchIrcServer::dropSeventvChannel(const QString &userID,
                                         const QString &emoteSetID)
{
    if (!getApp()->getSeventvEventAPI())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(this->channelMutex);

    // ignore empty values
    bool skipUser = userID.isEmpty();
    bool skipSet = emoteSetID.isEmpty();

    bool foundUser = skipUser;
    bool foundSet = skipSet;
    for (std::weak_ptr<Channel> &weak : this->channels)
    {
        ChannelPtr chan = weak.lock();
        if (!chan)
        {
            continue;
        }

        auto *channel = dynamic_cast<TwitchChannel *>(chan.get());
        if (!foundSet && channel->seventvEmoteSetID() == emoteSetID)
        {
            foundSet = true;
        }
        if (!foundUser && channel->seventvUserID() == userID)
        {
            foundUser = true;
        }

        if (foundSet && foundUser)
        {
            break;
        }
    }

    if (!foundUser)
    {
        getApp()->getSeventvEventAPI()->unsubscribeUser(userID);
    }
    if (!foundSet)
    {
        getApp()->getSeventvEventAPI()->unsubscribeEmoteSet(emoteSetID);
    }
}

void TwitchIrcServer::addFakeMessage(const QString &data)
{
    assertInGuiThread();

    auto *fakeMessage = Communi::IrcMessage::fromData(
        data.toUtf8(), this->writeConnection_.get());

    if (fakeMessage->command() == "PRIVMSG")
    {
        this->privateMessageReceived(
            static_cast<Communi::IrcPrivateMessage *>(fakeMessage));
    }
    else
    {
        this->readConnectionMessageReceived(fakeMessage);
    }
}

void TwitchIrcServer::addGlobalSystemMessage(const QString &messageText)
{
    std::lock_guard<std::mutex> lock(this->channelMutex);

    MessageBuilder b(systemMessage, messageText);
    auto message = b.release();

    for (std::weak_ptr<Channel> &weak : this->channels.values())
    {
        std::shared_ptr<Channel> chan = weak.lock();
        if (!chan)
        {
            continue;
        }

        chan->addMessage(message, MessageContext::Original);
    }
}

void TwitchIrcServer::forEachChannel(std::function<void(ChannelPtr)> func)
{
    std::lock_guard<std::mutex> lock(this->channelMutex);

    for (std::weak_ptr<Channel> &weak : this->channels.values())
    {
        ChannelPtr chan = weak.lock();
        if (!chan)
        {
            continue;
        }

        func(chan);
    }
}

void TwitchIrcServer::connect()
{
    assertInGuiThread();

    if (auto *provider = getApp()->getMoltorinoSupporterBadges())
    {
        provider->refreshPassive();
    }

    this->disconnect();

    TwitchIrcServer::initializeConnection(this->writeConnection_.get(),
                                          ConnectionType::Write);
    this->readConnection_->reconnect();

    this->refreshModeratedChannels();
}

void TwitchIrcServer::disconnect()
{
    std::lock_guard<std::mutex> locker(this->connectionMutex_);

    this->readConnection_->close();
    this->writeConnection_->close();
}

void TwitchIrcServer::sendMessage(const QString &channelName,
                                  const QString &message)
{
    this->sendRawMessage(makePrivmsg(channelName, message, makeIrcTags()));
}

void TwitchIrcServer::sendRawMessage(const QString &rawMessage)
{
    std::lock_guard<std::mutex> locker(this->connectionMutex_);

    this->writeConnection_->sendRaw(rawMessage);
}

ChannelPtr TwitchIrcServer::getOrAddChannel(const QString &dirtyChannelName)
{
    auto channelName = cleanChannelName(dirtyChannelName);

    if (auto custom = this->getCustomChannel(channelName))
    {
        return custom;
    }

    {
        std::lock_guard<std::mutex> lock(this->channelMutex);

        auto it = this->channels.find(channelName);
        if (it != this->channels.end())
        {
            if (auto chan = it.value().lock())
            {
                return chan;
            }
        }
    }

    std::lock_guard<std::mutex> lock(this->channelMutex);

    ChannelPtr chan = this->createChannel(channelName);
    auto *twitchChannel = dynamic_cast<TwitchChannel *>(chan.get());
    if (!chan || !twitchChannel)
    {
        return Channel::getEmpty();
    }

    this->channels.insert(channelName, chan);
    this->signalHolder.managedConnect(
        twitchChannel->destroyed, [this, channelName] {
            // fourtf: issues when the server itself is destroyed

            qCDebug(chatterinoIrc) << "[TwitchIrcServer::addChannel]"
                                   << channelName << "was destroyed";
            this->channels.remove(channelName);
        });

    // HACK(mm2pl): This prevents custom invalid twitch channels used by plugins from being joined
    if (!channelName.startsWith("/"))
    {
        this->readConnection_->onChannelCreated(*twitchChannel);
    }

    return chan;
}

ChannelPtr TwitchIrcServer::getOrAddAnonymousChannel(
    const QString &dirtyChannelName)
{
    auto channelName = cleanChannelName(dirtyChannelName);

    if (auto custom = this->getCustomChannel(channelName))
    {
        return custom;
    }

    {
        std::lock_guard<std::mutex> lock(this->channelMutex);

        auto it = this->anonymousChannels.find(channelName);
        if (it != this->anonymousChannels.end())
        {
            if (auto chan = it.value().lock())
            {
                return chan;
            }
        }
    }

    ChannelPtr chan;
    {
        std::lock_guard<std::mutex> lock(this->channelMutex);

        chan = this->createChannel(channelName, true);
        auto *twitchChannel = dynamic_cast<TwitchChannel *>(chan.get());
        if (!chan || !twitchChannel)
        {
            return Channel::getEmpty();
        }

        this->anonymousChannels.insert(channelName, chan);
        this->signalHolder.managedConnect(
            twitchChannel->destroyed, [this, channelName] {
                qCDebug(chatterinoIrc)
                    << "[TwitchIrcServer::addAnonymousChannel]" << channelName
                    << "was destroyed";
                this->anonymousChannels.remove(channelName);
                const bool hasAnonymousChannels =
                    !this->anonymousChannels.isEmpty();

                if (this->anonymousReadConnection_)
                {
                    if (!channelName.startsWith("/"))
                    {
                        this->anonymousReadConnection_->sendRaw("PART #" +
                                                                channelName);
                    }

                    if (!hasAnonymousChannels)
                    {
                        this->anonymousReadConnectionStarted_ = false;
                        this->anonymousReadConnection_->close();
                    }
                }
            });
    }

    this->ensureAnonymousReadConnection();

    {
        std::lock_guard<std::mutex> lock2(this->connectionMutex_);

        if (this->anonymousReadConnection_ &&
            this->anonymousReadConnection_->isConnected() &&
            !channelName.startsWith("/"))
        {
            this->anonymousJoinBucket_->send(channelName);
        }
    }

    return chan;
}

ChannelPtr TwitchIrcServer::getChannelOrEmpty(const QString &dirtyChannelName)
{
    auto channelName = cleanChannelName(dirtyChannelName);

    std::lock_guard<std::mutex> lock(this->channelMutex);

    // try get special channel
    ChannelPtr chan = this->getCustomChannel(channelName);
    if (chan)
    {
        return chan;
    }

    if (preferAnonymousTwitchChannels)
    {
        auto anonymous = this->anonymousChannels.find(channelName);
        if (anonymous != this->anonymousChannels.end())
        {
            chan = anonymous.value().lock();

            if (chan)
            {
                return chan;
            }
        }
    }

    // value exists
    auto it = this->channels.find(channelName);
    if (it != this->channels.end())
    {
        chan = it.value().lock();

        if (chan)
        {
            return chan;
        }
    }

    if (!preferAnonymousTwitchChannels)
    {
        auto anonymous = this->anonymousChannels.find(channelName);
        if (anonymous != this->anonymousChannels.end())
        {
            chan = anonymous.value().lock();

            if (chan)
            {
                return chan;
            }
        }
    }

    return Channel::getEmpty();
}

ChannelPtr TwitchIrcServer::getAnonymousChannelOrEmpty(
    const QString &dirtyChannelName)
{
    auto channelName = cleanChannelName(dirtyChannelName);

    std::lock_guard<std::mutex> lock(this->channelMutex);

    auto it = this->anonymousChannels.find(channelName);
    if (it != this->anonymousChannels.end())
    {
        if (auto chan = it.value().lock())
        {
            return chan;
        }
    }

    return Channel::getEmpty();
}

void TwitchIrcServer::reconnectAnonymousChannels()
{
    {
        std::lock_guard<std::mutex> lock(this->channelMutex);
        if (this->anonymousChannels.isEmpty())
        {
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lock(this->connectionMutex_);
        this->anonymousReadConnectionStarted_ = false;
        if (this->anonymousReadConnection_)
        {
            this->anonymousReadConnection_->close();
        }
    }

    QTimer::singleShot(0, this, [this] {
        this->ensureAnonymousReadConnection();
    });
}

bool TwitchIrcServer::isModeratorIn(const QString &broadcasterLogin) const
{
    return this->moderatedChannels.contains(broadcasterLogin);
}

void TwitchIrcServer::refreshModeratedChannels()
{
    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser->isAnon())
    {
        return;
    }
    CancellationToken token{false};
    this->moderatedChannelFetchToken = token;
    getHelix()->getModeratedChannels(
        currentUser->getUserId(),
        [this](auto &&set) {
            this->moderatedChannels = std::forward<decltype(set)>(set);
            this->applyModeratedChannelInfo();
        },
        [](const auto &message) {
            qCWarning(chatterinoTwitch)
                << "Failed to fetch moderated channels:" << message;
        },
        std::move(token));
}

void TwitchIrcServer::applyModeratedChannelInfo()
{
    if (this->moderatedChannels.empty())
    {
        return;
    }

    auto currentUserName =
        getApp()->getAccounts()->twitch.getCurrent()->getUserName();
    std::scoped_lock g(this->channelMutex);
    for (const auto &weak : std::as_const(this->channels))
    {
        if (auto chan = std::dynamic_pointer_cast<TwitchChannel>(weak.lock()))
        {
            if (chan->getName() != currentUserName)
            {
                chan->setMod(this->moderatedChannels.contains(chan->getName()));
            }
        }
    }
}

}  // namespace chatterino

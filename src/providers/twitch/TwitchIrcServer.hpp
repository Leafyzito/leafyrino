#pragma once

#include "common/Atomic.hpp"
#include "common/Channel.hpp"
#include "common/Common.hpp"
#include "providers/irc/IrcConnection2.hpp"
#include "util/RatelimitBucket.hpp"

#include <IrcMessage>
#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QRandomGenerator>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>

namespace chatterino {

class Settings;
class Paths;
class TwitchChannel;
class BttvEmotes;
class FfzEmotes;
class SeventvEmotes;
class RatelimitBucket;
class BttvLiveUpdates;
class SeventvEventAPI;

class ITwitchIrcServer
{
public:
    ITwitchIrcServer() = default;
    virtual ~ITwitchIrcServer() = default;
    ITwitchIrcServer(const ITwitchIrcServer &) = delete;
    ITwitchIrcServer(ITwitchIrcServer &&) = delete;
    ITwitchIrcServer &operator=(const ITwitchIrcServer &) = delete;
    ITwitchIrcServer &operator=(ITwitchIrcServer &&) = delete;

    virtual void connect() = 0;

    virtual void sendMessage(const QString &channelName,
                             const QString &message) = 0;
    virtual void sendRawMessage(const QString &rawMessage) = 0;

    virtual ChannelPtr getOrAddChannel(const QString &dirtyChannelName) = 0;
    virtual ChannelPtr getOrAddAnonymousChannel(
        const QString &dirtyChannelName) = 0;
    virtual ChannelPtr getChannelOrEmpty(const QString &dirtyChannelName) = 0;
    virtual ChannelPtr getAnonymousChannelOrEmpty(
        const QString &dirtyChannelName) = 0;
    virtual void reconnectAnonymousChannels() = 0;

    virtual void addFakeMessage(const QString &data) = 0;

    virtual void addGlobalSystemMessage(const QString &messageText) = 0;

    virtual void forEachChannel(std::function<void(ChannelPtr)> func) = 0;

    virtual void forEachChannelAndSpecialChannels(
        std::function<void(ChannelPtr)> func) = 0;

    virtual std::shared_ptr<Channel> getChannelOrEmptyByID(
        const QString &channelID) = 0;

    virtual void dropSeventvChannel(const QString &userID,
                                    const QString &emoteSetID) = 0;

    virtual const IndirectChannel &getWatchingChannel() const = 0;
    virtual void setWatchingChannel(ChannelPtr newWatchingChannel) = 0;
    virtual ChannelPtr getWhispersChannel() const = 0;
    virtual ChannelPtr getMentionsChannel() const = 0;
    virtual ChannelPtr getLiveChannel() const = 0;
    virtual ChannelPtr getAutomodChannel() const = 0;

    virtual QString getLastUserThatWhisperedMe() const = 0;
    virtual void setLastUserThatWhisperedMe(const QString &user) = 0;

    virtual void initEventAPIs(BttvLiveUpdates *bttvLiveUpdates,
                               SeventvEventAPI *seventvEventAPI) = 0;

};

class TwitchIrcServer final : public ITwitchIrcServer, public QObject
{
public:
    enum class ConnectionType {
        Read,
        Write,
        AnonymousRead,
    };

    TwitchIrcServer();
    ~TwitchIrcServer() override = default;

    TwitchIrcServer(const TwitchIrcServer &) = delete;
    TwitchIrcServer(TwitchIrcServer &&) = delete;
    TwitchIrcServer &operator=(const TwitchIrcServer &) = delete;
    TwitchIrcServer &operator=(TwitchIrcServer &&) = delete;

    void initialize();

    void aboutToQuit();

    void forEachChannelAndSpecialChannels(
        std::function<void(ChannelPtr)> func) override;

    std::shared_ptr<Channel> getChannelOrEmptyByID(
        const QString &channelID) override;

    void reloadAllBTTVChannelEmotes();
    void reloadAllFFZChannelEmotes();
    void reloadAllSevenTVChannelEmotes();

    void forEachSeventvEmoteSet(const QString &emoteSetId,
                                std::function<void(TwitchChannel &)> func);

    void forEachSeventvUser(const QString &userId,
                            std::function<void(TwitchChannel &)> func);

    void dropSeventvChannel(const QString &userID,
                            const QString &emoteSetID) override;

    void addFakeMessage(const QString &data) override;

    void addGlobalSystemMessage(const QString &messageText) override;

    void forEachChannel(std::function<void(ChannelPtr)> func) override;

    void connect() override;
    void disconnect();

    void sendMessage(const QString &channelName,
                     const QString &message) override;
    void sendRawMessage(const QString &rawMessage) override;

    ChannelPtr getOrAddChannel(const QString &dirtyChannelName) override;
    ChannelPtr getOrAddAnonymousChannel(
        const QString &dirtyChannelName) override;

    ChannelPtr getChannelOrEmpty(const QString &dirtyChannelName) override;
    ChannelPtr getAnonymousChannelOrEmpty(
        const QString &dirtyChannelName) override;
    void reconnectAnonymousChannels() override;

    void open(ConnectionType type);

private:
    Atomic<QString> lastUserThatWhisperedMe;

    const ChannelPtr whispersChannel;
    const ChannelPtr mentionsChannel;
    const ChannelPtr liveChannel;
    const ChannelPtr automodChannel;
    IndirectChannel watchingChannel;

public:
    const IndirectChannel &getWatchingChannel() const override;
    void setWatchingChannel(ChannelPtr newWatchingChannel) override;
    ChannelPtr getWhispersChannel() const override;
    ChannelPtr getMentionsChannel() const override;
    ChannelPtr getLiveChannel() const override;
    ChannelPtr getAutomodChannel() const override;

    QString getLastUserThatWhisperedMe() const override;
    void setLastUserThatWhisperedMe(const QString &user) override;

    void initEventAPIs(BttvLiveUpdates *bttvLiveUpdates,
                       SeventvEventAPI *seventvEventAPI) override;

protected:
    void initializeConnection(IrcConnection *connection, ConnectionType type);
    std::shared_ptr<Channel> createChannel(const QString &channelName,
                                           bool anonymous = false);

    void privateMessageReceived(Communi::IrcPrivateMessage *message,
                                bool anonymous = false);
    void readConnectionMessageReceived(Communi::IrcMessage *message,
                                       bool anonymous = false);
    void writeConnectionMessageReceived(Communi::IrcMessage *message);

    void onReadConnected(IrcConnection *connection);
    void onAnonymousReadConnected(IrcConnection *connection);
    void onWriteConnected(IrcConnection *connection);
    void onDisconnected();
    void onAnonymousDisconnected();
    void markChannelsConnected();
    void markAnonymousChannelsConnected();
    void ensureAnonymousReadConnection();

    std::shared_ptr<Channel> getCustomChannel(const QString &channelname);

private:
    void onMessageSendRequested(const std::shared_ptr<TwitchChannel> &channel,
                                const QString &message, bool &sent);
    void onReplySendRequested(const std::shared_ptr<TwitchChannel> &channel,
                              const QString &message, const QString &replyId,
                              bool &sent);

    bool prepareToSend(const std::shared_ptr<TwitchChannel> &channel);

    QMap<QString, std::weak_ptr<Channel>> channels;
    QMap<QString, std::weak_ptr<Channel>> anonymousChannels;
    std::mutex channelMutex;

    QObjectPtr<IrcConnection> writeConnection_ = nullptr;
    QObjectPtr<IrcConnection> readConnection_ = nullptr;
    QObjectPtr<IrcConnection> anonymousReadConnection_ = nullptr;
    bool anonymousReadConnectionStarted_ = false;

    QObjectPtr<RatelimitBucket> joinBucket_;
    QObjectPtr<RatelimitBucket> anonymousJoinBucket_;

    QTimer reconnectTimer_;
    int falloffCounter_ = 1;

    std::mutex connectionMutex_;

    pajlada::Signals::SignalHolder signalHolder;

    std::mutex lastMessageMutex_;
    std::queue<std::chrono::steady_clock::time_point> lastMessagePleb_;
    std::queue<std::chrono::steady_clock::time_point> lastMessageMod_;
    std::chrono::steady_clock::time_point lastErrorTimeSpeed_;
    std::chrono::steady_clock::time_point lastErrorTimeAmount_;

    QRandomGenerator generator;
};

}

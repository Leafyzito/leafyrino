// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Atomic.hpp"
#include "util/Expected.hpp"

#include <QString>
#include <QThread>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <vector>

class QTimer;

namespace chatterino::nm::detail {

ExpectedStr<void> writeManifestTo(QString directory, const QString &nmDirectory,
                                  const QString &filename,
                                  const QJsonDocument &json);

#ifndef Q_OS_WIN
/// Parse `path` by replacing '~', '$XDG_CONFIG_HOME' and '$XDG_DATA_HOME'
/// with their respective values.
/// Returns nullopt if the path is empty or relative.
std::optional<QString> parseCustomPath(QString path);
#endif

}  // namespace chatterino::nm::detail

namespace chatterino {

class Application;
class Paths;
class Channel;
class Modes;

using ChannelPtr = std::shared_ptr<Channel>;

void registerNmHost(Modes modes, const Paths &paths);
ExpectedStr<void> registerNmHost(const Paths &paths);
std::string &getNmQueueName(const Paths &paths);

Atomic<std::optional<QString>> &nmIpcError();

namespace nm::client {

void sendMessage(const QByteArray &array);
void writeToCout(const QByteArray &array);

}  // namespace nm::client

class NativeMessagingServer final
{
public:
    NativeMessagingServer();
    NativeMessagingServer(const NativeMessagingServer &) = delete;
    NativeMessagingServer(NativeMessagingServer &&) = delete;
    NativeMessagingServer &operator=(const NativeMessagingServer &) = delete;
    NativeMessagingServer &operator=(NativeMessagingServer &&) = delete;
    ~NativeMessagingServer();

    void start();

private:
    class ReceiverThread : public QThread
    {
    public:
        ReceiverThread(NativeMessagingServer &parent);

        void run() override;

    private:
        void handleMessage(const QJsonObject &root);
        void handleSelect(const QJsonObject &root);
        void handleDetach(const QJsonObject &root);
        void handleSync(const QJsonObject &root);

        NativeMessagingServer &parent_;
    };

    void syncChannels(const QJsonArray &twitchChannels);
    void noteActivity();

    ReceiverThread *thread;
    /// Steady-clock milliseconds of the last browser message that means the
    /// watching tab is still open. The receiver thread writes it; the detach
    /// timer reads it.
    std::atomic<std::chrono::milliseconds::rep> lastActivityMs_{0};
    std::unique_ptr<QTimer> detachTimer_;

    std::vector<ChannelPtr> channelWarmer_;

    friend ReceiverThread;
};

}  // namespace chatterino

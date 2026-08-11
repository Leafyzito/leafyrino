#pragma once

#include "util/QStringHash.hpp"

#include <boost/unordered/unordered_flat_map.hpp>
#include <QObject>

#include <memory>

namespace chatterino {

class Channel;
class YouTubeChannel;

class YouTubeChatServer : public QObject
{
public:
    YouTubeChatServer();
    ~YouTubeChatServer() override;

    Q_DISABLE_COPY_MOVE(YouTubeChatServer)

    void initialize();

    std::shared_ptr<Channel> getOrCreate(const QString &name);

    std::shared_ptr<YouTubeChannel> findByName(const QString &name) const;

private:
    boost::unordered_flat_map<QString, std::weak_ptr<YouTubeChannel>>
        channelsByName_;
};

}  // namespace chatterino

#include "providers/youtube/YouTubeChatServer.hpp"

#include "providers/youtube/YouTubeChannel.hpp"

namespace {

using namespace chatterino;

QString normalizeName(const QString &name)
{
    auto normalized = name.trimmed();
    if (normalized.startsWith('@'))
    {
        normalized = normalized.mid(1);
    }
    return normalized.toLower();
}

}  // namespace

namespace chatterino {

YouTubeChatServer::YouTubeChatServer() = default;

YouTubeChatServer::~YouTubeChatServer() = default;

void YouTubeChatServer::initialize()
{
}

std::shared_ptr<YouTubeChannel> YouTubeChatServer::findByName(
    const QString &name) const
{
    auto it = this->channelsByName_.find(normalizeName(name));
    if (it != this->channelsByName_.end())
    {
        return it->second.lock();
    }
    return nullptr;
}

std::shared_ptr<Channel> YouTubeChatServer::getOrCreate(const QString &name)
{
    auto existing = this->findByName(name);
    if (existing)
    {
        return existing;
    }

    auto displayName = name.trimmed();
    if (displayName.startsWith('@'))
    {
        displayName = displayName.mid(1);
    }

    auto chan = std::make_shared<YouTubeChannel>(displayName);
    this->channelsByName_[normalizeName(name)] = chan;
    chan->refreshLiveStream();
    return chan;
}

}  // namespace chatterino

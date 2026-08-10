#pragma once

#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"

#include <QColor>

namespace chatterino {

class YouTubeChannel;
struct YouTubeChatItem;

class YouTubeMessageBuilder : public MessageBuilder
{
public:
    YouTubeMessageBuilder(YouTubeChannel *channel, const QDateTime &time);

    static MessagePtrMut makeChatMessage(YouTubeChannel *channel,
                                         const YouTubeChatItem &item);

    YouTubeChannel *channel() const
    {
        return this->channel_;
    }

private:
    void appendChannelName();
    void appendUsername(const QString &username, const MessageColor &color);

    void buildSuperChat(const YouTubeChatItem &item, const QDateTime &time,
                        const QColor &userColor);
    void buildMembership(const YouTubeChatItem &item, const QDateTime &time,
                         const QColor &userColor);

    YouTubeChannel *channel_ = nullptr;
};

}  // namespace chatterino

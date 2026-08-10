#pragma once

#include "common/Channel.hpp"
#include "util/QStringHash.hpp"

#include <boost/unordered/unordered_flat_set.hpp>
#include <QString>
#include <QTimer>

#include <deque>
#include <memory>
#include <vector>

namespace chatterino {

struct YouTubeLiveStream;
struct YouTubeLiveChatPage;

class YouTubeChannel : public Channel
{
public:
    YouTubeChannel(const QString &name);
    ~YouTubeChannel() override;

    std::shared_ptr<YouTubeChannel> sharedFromThis();
    std::weak_ptr<YouTubeChannel> weakFromThis();

    const QString &getDisplayName() const override;
    bool isLive() const override;
    bool isWritable() const override;

    const QString &videoId() const;

    void refreshLiveStream();

    static void rememberAuthorPhoto(const QString &channelId,
                                    const QString &url);
    static QString authorPhotoFor(const QString &channelId);
    static QString channelIdForDisplayName(const ChannelPtr &channel,
                                           const QString &displayName);
    static QString normalizeDisplayName(const QString &displayName);

private:
    void startPolling(const YouTubeLiveStream &stream);
    void poll();

    void enqueueMessages(std::vector<MessagePtr> &messages, int windowMs);
    void drainChunk();
    void flushPending();

    void applyDeletions(const YouTubeLiveChatPage &page);

    bool markSeen(const QString &id);

    QString displayName_;
    QString videoId_;
    QString apiKey_;
    QString clientVersion_;
    QString continuation_;
    QString channelId_;

    bool live_ = false;
    bool resolving_ = false;
    bool firstBatch_ = true;

    QTimer pollTimer_;

    std::deque<MessagePtr> pendingMessages_;
    QTimer drainTimer_;
    int drainPerFrame_ = 1;
    int lastBatchSize_ = 0;

    boost::unordered_flat_set<QString> seenIds_;
    std::deque<QString> seenOrder_;
};

}  // namespace chatterino

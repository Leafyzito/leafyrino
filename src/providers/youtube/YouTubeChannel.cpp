#include "providers/youtube/YouTubeChannel.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "messages/Message.hpp"
#include "providers/youtube/YouTubeApi.hpp"
#include "providers/youtube/YouTubeMessageBuilder.hpp"
#include "singletons/WindowManager.hpp"

#include <boost/unordered/unordered_flat_map.hpp>
#include <QDateTime>

#include <algorithm>
#include <utility>

using namespace Qt::Literals::StringLiterals;

namespace chatterino {

namespace {

constexpr int DEFAULT_POLL_INTERVAL_MS = 1000;
constexpr int ERROR_POLL_INTERVAL_MS = 5000;

constexpr int POLL_INTERVAL_CAP_MS = 400;

constexpr int DRAIN_FRAME_MS = 16;
constexpr int MAX_LATENCY_MS = 4000;
constexpr int LIVE_LATENCY_MS = 2500;
constexpr int DRAIN_PER_FRAME_MAX = 200;

constexpr size_t SEEN_ID_CAP = 8000;

}  // namespace

namespace {

boost::unordered_flat_map<QString, QString> &authorPhotoRegistry()
{
    static boost::unordered_flat_map<QString, QString> registry;
    return registry;
}

}  // namespace

void YouTubeChannel::rememberAuthorPhoto(const QString &channelId,
                                         const QString &url)
{
    if (channelId.isEmpty() || url.isEmpty())
    {
        return;
    }
    authorPhotoRegistry()[channelId] = url;
}

QString YouTubeChannel::authorPhotoFor(const QString &channelId)
{
    auto &registry = authorPhotoRegistry();
    auto it = registry.find(channelId);
    if (it != registry.end())
    {
        return it->second;
    }
    return {};
}

QString YouTubeChannel::normalizeDisplayName(const QString &displayName)
{
    QString normalized = displayName.trimmed();
    if (normalized.startsWith(u'@'))
    {
        normalized = normalized.mid(1);
    }
    return normalized.toLower();
}

QString YouTubeChannel::channelIdForDisplayName(const ChannelPtr &channel,
                                                const QString &displayName)
{
    if (!channel)
    {
        return {};
    }
    const QString needle = normalizeDisplayName(displayName);
    if (needle.isEmpty())
    {
        return {};
    }
    const auto snapshot = channel->getMessageSnapshot();
    QString startsWithMatch;
    for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it)
    {
        const auto &message = *it;
        if (message->loginName.isEmpty())
        {
            continue;
        }
        const auto name = normalizeDisplayName(message->displayName);
        if (name == needle)
        {
            return message->loginName;
        }
        if (startsWithMatch.isEmpty() && name.startsWith(needle))
        {
            startsWithMatch = message->loginName;
        }
    }
    return startsWithMatch;
}

YouTubeChannel::YouTubeChannel(const QString &name)
    : Channel(name, Type::YouTube)
    , displayName_(name)
{
    this->pollTimer_.setSingleShot(true);
    QObject::connect(&this->pollTimer_, &QTimer::timeout, [this] {
        this->poll();
    });

    this->drainTimer_.setSingleShot(false);
    this->drainTimer_.setInterval(DRAIN_FRAME_MS);
    QObject::connect(&this->drainTimer_, &QTimer::timeout, [this] {
        this->drainChunk();
    });
}

YouTubeChannel::~YouTubeChannel() = default;

std::shared_ptr<YouTubeChannel> YouTubeChannel::sharedFromThis()
{
    return std::static_pointer_cast<YouTubeChannel>(this->shared_from_this());
}

std::weak_ptr<YouTubeChannel> YouTubeChannel::weakFromThis()
{
    return this->sharedFromThis();
}

const QString &YouTubeChannel::getDisplayName() const
{
    return this->displayName_;
}

bool YouTubeChannel::isLive() const
{
    return this->live_;
}

bool YouTubeChannel::isWritable() const
{
    return false;
}

const QString &YouTubeChannel::videoId() const
{
    return this->videoId_;
}

void YouTubeChannel::refreshLiveStream()
{
    if (this->resolving_)
    {
        return;
    }
    this->resolving_ = true;
    this->pollTimer_.stop();
    this->addSystemMessage(u"Refreshing YouTube chat..."_s);

    YouTubeApi::resolveLiveStream(
        this->getName(), [weak = this->weakFromThis()](
                             const ExpectedStr<YouTubeLiveStream> &res) {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            self->resolving_ = false;
            if (!res)
            {
                self->live_ = false;
                self->addSystemMessage(u"Could not refresh YouTube chat."_s);
                return;
            }
            if (res->continuation.isEmpty())
            {
                self->live_ = false;
                self->addSystemMessage(u"Channel offline."_s);
                return;
            }
            self->startPolling(*res);
            self->addSystemMessage(u"Done. Watching the latest live."_s);
        });
}

void YouTubeChannel::startPolling(const YouTubeLiveStream &stream)
{
    this->drainTimer_.stop();
    this->pendingMessages_.clear();
    this->seenIds_.clear();
    this->seenOrder_.clear();
    this->lastBatchSize_ = 0;
    this->firstBatch_ = true;

    this->videoId_ = stream.videoId;
    this->apiKey_ = stream.apiKey;
    this->clientVersion_ = stream.clientVersion;
    this->continuation_ = stream.continuation;
    this->channelId_ = stream.channelId;
    if (!stream.channelName.isEmpty())
    {
        this->displayName_ = stream.channelName;
        this->displayNameChanged.invoke();
    }
    this->live_ = true;
    this->poll();
}

void YouTubeChannel::poll()
{
    if (this->apiKey_.isEmpty() || this->continuation_.isEmpty())
    {
        this->live_ = false;
        return;
    }

    YouTubeApi::fetchLiveChat(
        this->apiKey_, this->clientVersion_, this->continuation_,
        [weak = this->weakFromThis()](
            const ExpectedStr<YouTubeLiveChatPage> &res) {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            if (!res)
            {
                qCWarning(chatterinoYouTube)
                    << "Live chat poll failed:" << res.error();
                self->pollTimer_.start(ERROR_POLL_INTERVAL_MS);
                return;
            }

            std::vector<MessagePtr> messages;
            messages.reserve(res->items.size());
            for (const auto &item : res->items)
            {
                if (!self->markSeen(item.id))
                {
                    continue;
                }
                auto msg =
                    YouTubeMessageBuilder::makeChatMessage(self.get(), item);
                if (msg)
                {
                    messages.push_back(std::move(msg));
                }
            }

            const int timeoutMs =
                res->timeoutMs > 0 ? res->timeoutMs : DEFAULT_POLL_INTERVAL_MS;
            const bool ended = res->ended || res->continuation.isEmpty();

            if (self->firstBatch_)
            {
                self->firstBatch_ = false;
                if (!messages.empty())
                {
                    self->addMessagesAtStart(messages);
                }
            }
            else if (!messages.empty())
            {
                self->enqueueMessages(
                    messages, std::min(timeoutMs, POLL_INTERVAL_CAP_MS));
            }
            self->lastBatchSize_ = static_cast<int>(messages.size());

            self->applyDeletions(*res);

            if (ended)
            {
                self->flushPending();
                self->live_ = false;
                self->addSystemMessage(u"The YouTube live chat has ended."_s);
                return;
            }

            self->continuation_ = res->continuation;
            self->pollTimer_.start(std::min(timeoutMs, POLL_INTERVAL_CAP_MS));
        });
}

void YouTubeChannel::enqueueMessages(std::vector<MessagePtr> &messages,
                                     int windowMs)
{
    const size_t backlog = this->pendingMessages_.size();

    for (auto &msg : messages)
    {
        this->pendingMessages_.push_back(std::move(msg));
    }

    int effectiveWindowMs =
        std::clamp(windowMs, DRAIN_FRAME_MS, MAX_LATENCY_MS);

    if (this->lastBatchSize_ > 0 &&
        backlog > static_cast<size_t>(this->lastBatchSize_) * 3 / 2)
    {
        effectiveWindowMs = std::max(DRAIN_FRAME_MS, effectiveWindowMs / 2);
    }

    const int targetFrames = std::max(1, effectiveWindowMs / DRAIN_FRAME_MS);
    const auto perFrame = static_cast<int>(
        (this->pendingMessages_.size() + targetFrames - 1) / targetFrames);
    this->drainPerFrame_ = std::clamp(perFrame, 1, DRAIN_PER_FRAME_MAX);

    const size_t maxKeep = static_cast<size_t>(this->drainPerFrame_) *
                           (LIVE_LATENCY_MS / DRAIN_FRAME_MS);
    if (this->pendingMessages_.size() > maxKeep)
    {
        const size_t dropCount = this->pendingMessages_.size() - maxKeep;
        for (size_t i = 0; i < dropCount; i++)
        {
            this->pendingMessages_.pop_front();
        }
        qCDebug(chatterinoYouTube)
            << "Dropping" << dropCount
            << "oldest queued messages to stay live (backlog outpaced drain).";
    }

    if (!this->pendingMessages_.empty() && !this->drainTimer_.isActive())
    {
        this->drainTimer_.start();
    }
}

void YouTubeChannel::applyDeletions(const YouTubeLiveChatPage &page)
{
    if (page.deletedItemIds.empty() && page.deletedAuthorChannelIds.empty())
    {
        return;
    }

    bool changed = false;

    for (const auto &targetId : page.deletedItemIds)
    {
        const QString fullId = u"yt-"_s % targetId;
        if (auto msg = this->findMessageByID(fullId))
        {
            msg->flags.set(MessageFlag::Disabled);
            changed = true;
        }
        for (auto &pending : this->pendingMessages_)
        {
            if (pending->id == fullId)
            {
                pending->flags.set(MessageFlag::Disabled);
                changed = true;
            }
        }
    }

    if (!page.deletedAuthorChannelIds.empty())
    {
        auto matchesAuthor = [&](const MessagePtr &msg) {
            for (const auto &channelId : page.deletedAuthorChannelIds)
            {
                if (msg->loginName == channelId)
                {
                    return true;
                }
            }
            return false;
        };

        for (const auto &msg : this->getMessageSnapshot())
        {
            if (msg->flags.has(MessageFlag::System))
            {
                continue;
            }
            if (matchesAuthor(msg))
            {
                msg->flags.set(MessageFlag::Disabled);
                changed = true;
            }
        }
        for (auto &pending : this->pendingMessages_)
        {
            if (matchesAuthor(pending))
            {
                pending->flags.set(MessageFlag::Disabled);
                changed = true;
            }
        }
    }

    if (changed)
    {
        getApp()->getWindows()->forceLayoutChannelViews();
    }
}

bool YouTubeChannel::markSeen(const QString &id)
{
    if (id.isEmpty())
    {
        return true;  // can't de-dupe without an id; let it through
    }

    auto [it, inserted] = this->seenIds_.insert(id);
    if (!inserted)
    {
        return false;
    }

    this->seenOrder_.push_back(id);
    if (this->seenOrder_.size() > SEEN_ID_CAP)
    {
        this->seenIds_.erase(this->seenOrder_.front());
        this->seenOrder_.pop_front();
    }
    return true;
}

void YouTubeChannel::drainChunk()
{
    if (this->pendingMessages_.empty())
    {
        this->drainTimer_.stop();
        return;
    }

    for (int i = 0; i < this->drainPerFrame_ && !this->pendingMessages_.empty();
         i++)
    {
        auto msg = std::move(this->pendingMessages_.front());
        this->pendingMessages_.pop_front();
        this->addMessage(msg, MessageContext::Original);
    }

    if (this->pendingMessages_.empty())
    {
        this->drainTimer_.stop();
    }
}

void YouTubeChannel::flushPending()
{
    this->drainTimer_.stop();
    while (!this->pendingMessages_.empty())
    {
        auto msg = std::move(this->pendingMessages_.front());
        this->pendingMessages_.pop_front();
        this->addMessage(msg, MessageContext::Original);
    }
}

}  // namespace chatterino

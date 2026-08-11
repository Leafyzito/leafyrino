#pragma once

#include "util/Expected.hpp"

#include <QString>

#include <cstdint>
#include <functional>
#include <vector>

namespace chatterino {

struct YouTubeLiveStream {
    QString videoId;
    QString apiKey;
    QString clientVersion;
    QString continuation;
    QString channelId;
    QString channelName;

    QString title;
    QString author;
    QString handle;
    QString thumbnailUrl;
    int viewerCount = -1;
    bool isLive = false;
};

struct YouTubeMetadata {
    QString title;
    int viewerCount = -1;
    bool hasTitle = false;
    bool hasViewerCount = false;
    int timeoutMs = 0;
};

enum class YouTubeRunKind : uint8_t {
    Text,
    Emoji,
};

struct YouTubeMessageRun {
    YouTubeRunKind kind = YouTubeRunKind::Text;
    QString text;
    QString imageUrl;
    bool isCustomEmoji = false;
};

enum class YouTubeChatItemKind : uint8_t {
    Text,
    Gift,
    Membership,
    SuperChat,
    SuperSticker,
    Unsupported,
};

enum class YouTubeAuthorBadgeKind : uint8_t {
    Member,
    Moderator,
    Owner,
    Verified,
};

struct YouTubeAuthorBadge {
    YouTubeAuthorBadgeKind kind = YouTubeAuthorBadgeKind::Member;
    QString imageUrl;
    QString tooltip;
};

struct YouTubeChatItem {
    YouTubeChatItemKind kind = YouTubeChatItemKind::Unsupported;
    QString id;
    QString authorName;
    QString authorChannelId;
    QString authorPhoto;
    int64_t timestampUsec = 0;
    std::vector<YouTubeMessageRun> runs;
    std::vector<YouTubeAuthorBadge> authorBadges;

    QString amountText;
    QString stickerUrl;
    uint32_t bodyBackgroundColor = 0;
    uint32_t authorNameTextColor = 0;
    uint32_t headerBackgroundColor = 0;
};

struct YouTubeLiveChatPage {
    std::vector<YouTubeChatItem> items;
    QString continuation;
    int timeoutMs = 0;
    bool ended = false;

    std::vector<QString> deletedItemIds;
    std::vector<QString> deletedAuthorChannelIds;
};

class YouTubeApi
{
public:
    template <typename T>
    using Callback = std::function<void(ExpectedStr<T>)>;

    static void resolveLiveStream(const QString &channel,
                                  Callback<YouTubeLiveStream> cb);

    static void fetchLiveChat(const QString &apiKey,
                              const QString &clientVersion,
                              const QString &continuation,
                              Callback<YouTubeLiveChatPage> cb);

    static void fetchUpdatedMetadata(const QString &apiKey,
                                     const QString &clientVersion,
                                     const QString &videoId,
                                     Callback<YouTubeMetadata> cb);

    static void fetchWatchMetadata(const QString &videoId,
                                   Callback<YouTubeMetadata> cb);
};

}  // namespace chatterino

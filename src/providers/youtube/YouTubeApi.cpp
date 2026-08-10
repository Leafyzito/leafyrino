#include "providers/youtube/YouTubeApi.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "util/BoostJsonWrap.hpp"

#include <boost/json/parse.hpp>
#include <QJsonObject>
#include <QRegularExpression>

namespace {

using namespace chatterino;
using namespace Qt::Literals::StringLiterals;

const QString USER_AGENT =
    u"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, "
    u"like Gecko) Chrome/122.0.0.0 Safari/537.36"_s;
const QString ACCEPT_LANGUAGE = u"en-US,en;q=0.9"_s;
// YouTube serves a GDPR consent interstitial without this (EU/EEA).
// "CAI" = accept-all SOCS value used by yt-dlp and similar clients.
const QString CONSENT_COOKIE = u"SOCS=CAI"_s;

QString buildLiveUrl(const QString &channel)
{
    auto trimmed = channel.trimmed();
    if (trimmed.startsWith(u"http://") || trimmed.startsWith(u"https://"))
    {
        return trimmed;
    }
    if (trimmed.startsWith('@'))
    {
        trimmed = trimmed.mid(1);
    }
    if (trimmed.startsWith(u"UC") && trimmed.size() == 24)
    {
        return u"https://www.youtube.com/channel/" % trimmed % u"/live";
    }
    return u"https://www.youtube.com/@" % trimmed % u"/live";
}

QString firstCapture(const QRegularExpression &re, const QString &haystack)
{
    auto match = re.match(haystack);
    if (match.hasMatch())
    {
        return match.captured(1);
    }
    return {};
}

YouTubeLiveStream scrapeLiveStream(const QString &html)
{
    static const QRegularExpression apiKeyRe(
        uR"re("INNERTUBE_API_KEY":"([^"]+)")re"_s);
    static const QRegularExpression clientVersionRe(
        uR"re("INNERTUBE_CONTEXT_CLIENT_VERSION":"([^"]+)")re"_s);
    static const QRegularExpression clientVersionAltRe(
        uR"re("clientVersion":"([^"]+)")re"_s);
    static const QRegularExpression canonicalVideoRe(
        uR"re(rel="canonical" href="https://www\.youtube\.com/watch\?v=([\w-]+)")re"_s);
    static const QRegularExpression videoIdRe(uR"re("videoId":"([\w-]+)")re"_s);
    static const QRegularExpression channelIdRe(
        uR"re("channelId":"(UC[\w-]+)")re"_s);
    static const QRegularExpression channelNameRe(
        uR"re("channelName":"([^"]+)")re"_s);
    static const QRegularExpression continuationRe(
        uR"re("continuation":"([^"]+)")re"_s);

    YouTubeLiveStream stream;
    stream.apiKey = firstCapture(apiKeyRe, html);
    stream.clientVersion = firstCapture(clientVersionRe, html);
    if (stream.clientVersion.isEmpty())
    {
        stream.clientVersion = firstCapture(clientVersionAltRe, html);
    }
    stream.videoId = firstCapture(canonicalVideoRe, html);
    if (stream.videoId.isEmpty())
    {
        stream.videoId = firstCapture(videoIdRe, html);
    }
    stream.channelId = firstCapture(channelIdRe, html);
    stream.channelName = firstCapture(channelNameRe, html);

    auto chatIndex = html.indexOf(u"liveChatRenderer");
    if (chatIndex < 0)
    {
        chatIndex = html.indexOf(u"liveChatContinuation");
    }
    if (chatIndex >= 0)
    {
        stream.continuation = firstCapture(continuationRe, html.mid(chatIndex));
    }

    return stream;
}

QString scrapeLiveChatContinuation(const QString &html)
{
    static const QRegularExpression liveRe(
        uR"re("selected":false,"continuation":\{"reloadContinuationData":\{"continuation":"([^"]+)")re"_s);
    auto match = liveRe.match(html);
    if (match.hasMatch())
    {
        return match.captured(1);
    }

    static const QRegularExpression reloadRe(
        uR"re("reloadContinuationData":\{"continuation":"([^"]+)")re"_s);
    auto it = reloadRe.globalMatch(html);
    QString first;
    QString second;
    for (int i = 0; it.hasNext() && i < 2; i++)
    {
        auto m = it.next();
        (i == 0 ? first : second) = m.captured(1);
    }
    if (!second.isEmpty())
    {
        return second;
    }
    return first;
}

QString extractText(BoostJsonValue value)
{
    auto simple = value["simpleText"];
    if (simple.isString())
    {
        return simple.toQString();
    }
    QString out;
    for (auto run : value["runs"].toArray())
    {
        out += run["text"].toQString();
    }
    return out;
}

QString lastThumbnailUrl(BoostJsonValue thumbnails)
{
    auto arr = thumbnails.toArray();
    if (arr.empty())
    {
        return {};
    }
    return arr[arr.size() - 1]["url"].toQString();
}

YouTubeMessageRun parseRun(BoostJsonValue run)
{
    YouTubeMessageRun out;
    auto emoji = run["emoji"];
    if (!emoji.isObject())
    {
        out.kind = YouTubeRunKind::Text;
        out.text = run["text"].toQString();
        return out;
    }

    auto emojiId = emoji["emojiId"].toQString();
    const bool isCustom =
        emoji["isCustomEmoji"].toBool() || emojiId.contains('/');

    if (!isCustom)
    {
        out.kind = YouTubeRunKind::Text;
        auto shortcuts = emoji["shortcuts"].toArray();
        out.text = emojiId;
        if (out.text.isEmpty() && !shortcuts.empty())
        {
            out.text = shortcuts[0].toQString();
        }
        return out;
    }

    out.kind = YouTubeRunKind::Emoji;
    out.isCustomEmoji = true;
    auto thumbnails = emoji["image"]["thumbnails"].toArray();
    if (!thumbnails.empty())
    {
        out.imageUrl = thumbnails[thumbnails.size() - 1]["url"].toQString();
    }

    auto shortcuts = emoji["shortcuts"].toArray();
    if (!shortcuts.empty())
    {
        out.text = shortcuts[0].toQString();
    }
    if (out.text.isEmpty())
    {
        auto searchTerms = emoji["searchTerms"].toArray();
        if (!searchTerms.empty())
        {
            out.text = searchTerms[0].toQString();
        }
    }
    if (out.text.isEmpty())
    {
        out.text = emojiId;
    }
    return out;
}

void parseAuthorBadges(BoostJsonValue badges,
                       std::vector<YouTubeAuthorBadge> &out)
{
    for (auto badge : badges.toArray())
    {
        auto renderer = badge["liveChatAuthorBadgeRenderer"];
        if (!renderer.isObject())
        {
            continue;
        }
        auto tooltip = renderer["tooltip"].toQString();
        auto iconType = renderer["icon"]["iconType"].toQString();
        if (!iconType.isEmpty())
        {
            YouTubeAuthorBadge b;
            b.tooltip = tooltip;
            if (iconType == u"MODERATOR")
            {
                b.kind = YouTubeAuthorBadgeKind::Moderator;
            }
            else if (iconType == u"OWNER")
            {
                b.kind = YouTubeAuthorBadgeKind::Owner;
            }
            else if (iconType == u"VERIFIED")
            {
                b.kind = YouTubeAuthorBadgeKind::Verified;
            }
            else
            {
                continue;
            }
            out.push_back(std::move(b));
        }
        else
        {
            auto thumbnails =
                renderer["customThumbnail"]["thumbnails"].toArray();
            if (thumbnails.empty())
            {
                continue;
            }
            YouTubeAuthorBadge b;
            b.kind = YouTubeAuthorBadgeKind::Member;
            b.imageUrl = thumbnails[thumbnails.size() - 1]["url"].toQString();
            b.tooltip = tooltip;
            if (b.imageUrl.isEmpty())
            {
                continue;
            }
            out.push_back(std::move(b));
        }
    }
}

YouTubeChatItem parseTextRenderer(BoostJsonObject renderer)
{
    YouTubeChatItem item;
    item.kind = YouTubeChatItemKind::Text;
    item.id = renderer["id"].toQString();
    item.authorName = extractText(renderer["authorName"]);
    item.authorChannelId = renderer["authorExternalChannelId"].toQString();
    item.authorPhoto = lastThumbnailUrl(renderer["authorPhoto"]["thumbnails"]);
    item.timestampUsec = renderer["timestampUsec"].toQString().toLongLong();
    for (auto run : renderer["message"]["runs"].toArray())
    {
        item.runs.push_back(parseRun(run));
    }
    parseAuthorBadges(renderer["authorBadges"], item.authorBadges);
    return item;
}

YouTubeChatItem parseGiftRenderer(BoostJsonObject renderer)
{
    YouTubeChatItem item;
    item.kind = YouTubeChatItemKind::Gift;
    item.id = renderer["id"].toQString();
    item.timestampUsec = renderer["timestampUsec"].toQString().toLongLong();
    auto header = renderer["header"]["liveChatSponsorshipsHeaderRenderer"];
    item.authorName = extractText(header["authorName"]);
    item.authorChannelId = renderer["authorExternalChannelId"].toQString();
    for (auto run : header["primaryText"]["runs"].toArray())
    {
        item.runs.push_back(parseRun(run));
    }
    parseAuthorBadges(header["authorBadges"], item.authorBadges);
    return item;
}

YouTubeChatItem parseMembershipRenderer(BoostJsonObject renderer)
{
    YouTubeChatItem item;
    item.kind = YouTubeChatItemKind::Membership;
    item.id = renderer["id"].toQString();
    item.authorName = extractText(renderer["authorName"]);
    item.authorChannelId = renderer["authorExternalChannelId"].toQString();
    item.authorPhoto = lastThumbnailUrl(renderer["authorPhoto"]["thumbnails"]);
    item.timestampUsec = renderer["timestampUsec"].toQString().toLongLong();

    auto primary = renderer["headerPrimaryText"];
    auto subtext = renderer["headerSubtext"];
    const auto &header = primary.isObject() ? primary : subtext;
    if (header.isObject())
    {
        auto simple = header["simpleText"];
        if (simple.isString())
        {
            YouTubeMessageRun run;
            run.text = simple.toQString();
            item.runs.push_back(std::move(run));
        }
        else
        {
            for (auto run : header["runs"].toArray())
            {
                item.runs.push_back(parseRun(run));
            }
        }
    }
    for (auto run : renderer["message"]["runs"].toArray())
    {
        item.runs.push_back(parseRun(run));
    }
    parseAuthorBadges(renderer["authorBadges"], item.authorBadges);
    return item;
}

YouTubeChatItem parsePaidMessageRenderer(BoostJsonObject renderer)
{
    YouTubeChatItem item;
    item.kind = YouTubeChatItemKind::SuperChat;
    item.id = renderer["id"].toQString();
    item.authorName = extractText(renderer["authorName"]);
    item.authorChannelId = renderer["authorExternalChannelId"].toQString();
    item.authorPhoto = lastThumbnailUrl(renderer["authorPhoto"]["thumbnails"]);
    item.timestampUsec = renderer["timestampUsec"].toQString().toLongLong();
    item.amountText = extractText(renderer["purchaseAmountText"]);
    for (auto run : renderer["message"]["runs"].toArray())
    {
        item.runs.push_back(parseRun(run));
    }
    parseAuthorBadges(renderer["authorBadges"], item.authorBadges);
    item.bodyBackgroundColor =
        static_cast<uint32_t>(renderer["bodyBackgroundColor"].toUint64());
    item.authorNameTextColor =
        static_cast<uint32_t>(renderer["authorNameTextColor"].toUint64());
    item.headerBackgroundColor =
        static_cast<uint32_t>(renderer["headerBackgroundColor"].toUint64());
    return item;
}

YouTubeChatItem parsePaidStickerRenderer(BoostJsonObject renderer)
{
    YouTubeChatItem item;
    item.kind = YouTubeChatItemKind::SuperSticker;
    item.id = renderer["id"].toQString();
    item.authorName = extractText(renderer["authorName"]);
    item.authorChannelId = renderer["authorExternalChannelId"].toQString();
    item.timestampUsec = renderer["timestampUsec"].toQString().toLongLong();
    item.amountText = extractText(renderer["purchaseAmountText"]);
    auto thumbnails = renderer["sticker"]["thumbnails"].toArray();
    if (!thumbnails.empty())
    {
        item.stickerUrl = thumbnails[thumbnails.size() - 1]["url"].toQString();
        if (item.stickerUrl.startsWith(u"//"))
        {
            item.stickerUrl = u"https:" % item.stickerUrl;
        }
    }
    parseAuthorBadges(renderer["authorBadges"], item.authorBadges);
    item.bodyBackgroundColor =
        static_cast<uint32_t>(renderer["backgroundColor"].toUint64());
    item.headerBackgroundColor =
        static_cast<uint32_t>(renderer["moneyChipBackgroundColor"].toUint64());
    return item;
}

YouTubeLiveChatPage parseLiveChatPage(BoostJsonValue root)
{
    YouTubeLiveChatPage page;
    auto liveChat = root["continuationContents"]["liveChatContinuation"];

    for (auto action : liveChat["actions"].toArray())
    {
        auto deleteAction = action["markChatItemAsDeletedAction"];
        if (deleteAction.isObject())
        {
            auto targetId = deleteAction["targetItemId"].toQString();
            if (!targetId.isEmpty())
            {
                page.deletedItemIds.push_back(targetId);
            }
            continue;
        }
        auto deleteByAuthor = action["markChatItemsByAuthorAsDeletedAction"];
        if (deleteByAuthor.isObject())
        {
            auto channelId = deleteByAuthor["externalChannelId"].toQString();
            if (!channelId.isEmpty())
            {
                page.deletedAuthorChannelIds.push_back(channelId);
            }
            continue;
        }

        auto item = action["addChatItemAction"]["item"];
        auto textRenderer = item["liveChatTextMessageRenderer"];
        auto giftRenderer =
            item["liveChatSponsorshipsGiftPurchaseAnnouncementRenderer"];
        auto membershipRenderer = item["liveChatMembershipItemRenderer"];
        auto paidMessageRenderer = item["liveChatPaidMessageRenderer"];
        auto paidStickerRenderer = item["liveChatPaidStickerRenderer"];
        YouTubeChatItem chatItem;
        if (textRenderer.isObject())
        {
            chatItem = parseTextRenderer(textRenderer.toObject());
        }
        else if (paidMessageRenderer.isObject())
        {
            chatItem = parsePaidMessageRenderer(paidMessageRenderer.toObject());
        }
        else if (paidStickerRenderer.isObject())
        {
            chatItem = parsePaidStickerRenderer(paidStickerRenderer.toObject());
        }
        else if (giftRenderer.isObject())
        {
            chatItem = parseGiftRenderer(giftRenderer.toObject());
        }
        else if (membershipRenderer.isObject())
        {
            chatItem = parseMembershipRenderer(membershipRenderer.toObject());
        }
        else
        {
            continue;
        }
        if (!chatItem.id.isEmpty())
        {
            page.items.push_back(std::move(chatItem));
        }
    }

    auto continuations = liveChat["continuations"].toArray();
    if (!continuations.empty())
    {
        auto next = continuations[0];
        auto timed = next["timedContinuationData"];
        auto invalidation = next["invalidationContinuationData"];
        auto reload = next["reloadContinuationData"];
        if (timed.isObject())
        {
            page.continuation = timed["continuation"].toQString();
            page.timeoutMs = static_cast<int>(timed["timeoutMs"].toInt64());
        }
        else if (invalidation.isObject())
        {
            page.continuation = invalidation["continuation"].toQString();
            page.timeoutMs =
                static_cast<int>(invalidation["timeoutMs"].toInt64());
        }
        else if (reload.isObject())
        {
            page.continuation = reload["continuation"].toQString();
        }
    }

    page.ended = page.continuation.isEmpty();
    return page;
}

}  // namespace

namespace chatterino {

void YouTubeApi::resolveLiveStream(const QString &channel,
                                   Callback<YouTubeLiveStream> cb)
{
    NetworkRequest(buildLiveUrl(channel))
        .followRedirects(true)
        .header("User-Agent", USER_AGENT)
        .header("Accept-Language", ACCEPT_LANGUAGE)
        .header("Cookie", CONSENT_COOKIE)
        .timeout(20000)
        .onError([cb](const NetworkResult &res) {
            cb(makeUnexpected(res.formatError()));
        })
        .onSuccess([cb = std::move(cb)](const NetworkResult &res) {
            auto stream = scrapeLiveStream(QString::fromUtf8(res.getData()));
            if (stream.apiKey.isEmpty())
            {
                cb(makeUnexpected(
                    u"Could not read YouTube page (missing API key)."_s));
                return;
            }
            if (stream.videoId.isEmpty())
            {
                stream.continuation.clear();
                cb(std::move(stream));
                return;
            }

            QString popoutUrl =
                u"https://www.youtube.com/live_chat?is_popout=1&v=" %
                stream.videoId;
            NetworkRequest(popoutUrl)
                .followRedirects(true)
                .header("User-Agent", USER_AGENT)
                .header("Accept-Language", ACCEPT_LANGUAGE)
                .header("Cookie", CONSENT_COOKIE)
                .timeout(20000)
                .onError([cb, stream](const NetworkResult &) {
                    cb(stream);
                })
                .onSuccess([cb,
                            stream](const NetworkResult &popoutRes) mutable {
                    auto popout = QString::fromUtf8(popoutRes.getData());
                    auto liveContinuation = scrapeLiveChatContinuation(popout);
                    if (!liveContinuation.isEmpty())
                    {
                        stream.continuation = liveContinuation;
                        auto popoutStream = scrapeLiveStream(popout);
                        if (!popoutStream.apiKey.isEmpty())
                        {
                            stream.apiKey = popoutStream.apiKey;
                        }
                        if (!popoutStream.clientVersion.isEmpty())
                        {
                            stream.clientVersion = popoutStream.clientVersion;
                        }
                    }
                    cb(std::move(stream));
                })
                .execute();
        })
        .execute();
}

void YouTubeApi::fetchLiveChat(const QString &apiKey,
                               const QString &clientVersion,
                               const QString &continuation,
                               Callback<YouTubeLiveChatPage> cb)
{
    QString url =
        u"https://www.youtube.com/youtubei/v1/live_chat/get_live_chat?key=" %
        apiKey;

    QJsonObject client{
        {"clientVersion"_L1, clientVersion},
        {"clientName"_L1, "WEB"_L1},
    };
    QJsonObject body{
        {"context"_L1, QJsonObject{{"client"_L1, client}}},
        {"continuation"_L1, continuation},
    };

    NetworkRequest(url, NetworkRequestType::Post)
        .json(body)
        .header("User-Agent", USER_AGENT)
        .header("Accept-Language", ACCEPT_LANGUAGE)
        .header("Cookie", CONSENT_COOKIE)
        .timeout(15000)
        .onError([cb](const NetworkResult &res) {
            cb(makeUnexpected(res.formatError()));
        })
        .onSuccess([cb = std::move(cb)](const NetworkResult &res) {
            const auto &data = res.getData();
            boost::system::error_code ec;
            auto parsed = boost::json::parse(
                std::string_view(data.data(), data.size()), ec);
            if (ec)
            {
                cb(makeUnexpected(u"Failed to parse live chat response: "_s %
                                  QString::fromStdString(ec.message())));
                return;
            }
            BoostJsonValue root(parsed);
            cb(parseLiveChatPage(root));
        })
        .execute();
}

}  // namespace chatterino

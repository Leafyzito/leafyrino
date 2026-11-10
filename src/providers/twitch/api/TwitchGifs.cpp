#include "providers/twitch/api/TwitchGifs.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>

namespace chatterino::twitchgifs {
namespace {

NetworkRequest request(const QString &token, const char *query,
                       const QJsonObject &variables, const QObject *caller)
{
    return NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
        .header("Client-Id", MoltorinoAuth::twitchTvClientId())
        .header("Authorization", "OAuth " + token)
        .json(QJsonObject{{"query", query}, {"variables", variables}})
        .hideRequestBody()
        .followRedirects(false)
        .timeout(15000)
        .caller(caller);
}

QJsonObject responseObject(const NetworkResult &result)
{
    const auto json = result.parseJsonValue();
    if (json.isArray())
    {
        const auto array = json.toArray();
        return array.size() == 1 ? array.at(0).toObject() : QJsonObject{};
    }
    return json.toObject();
}

bool ratingAllowed(const QString &rating, const QString &maximumRating)
{
    if (rating == "g" || rating == "y")
    {
        return true;
    }
    if (rating == "pg")
    {
        return maximumRating != "g";
    }
    return maximumRating == "pg-13" && rating == "pg-13";
}

}  // namespace

bool validId(const QString &id)
{
    return !id.isEmpty() && id.size() <= 64 &&
           std::ranges::all_of(id, [](QChar c) {
               return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') ||
                      (c >= u'0' && c <= u'9');
           });
}

bool validGif(const Gif &gif)
{
    const QUrl url(gif.url);
    const auto host = url.host().toLower();
    return validId(gif.id) && !gif.title.isEmpty() && gif.title.size() <= 300 &&
           gif.url.size() <= 4096 && url.isValid() && url.scheme() == "https" &&
           url.userInfo().isEmpty() && url.port(443) == 443 &&
           (host == "media.giphy.com" || host == "media0.giphy.com" ||
            host == "media1.giphy.com" || host == "media2.giphy.com" ||
            host == "media3.giphy.com" || host == "media4.giphy.com") &&
           url.path().endsWith("/" + gif.id + "/giphy.gif");
}

QStringList favoriteIds(const QString &json)
{
    QStringList ids;

    if (json.size() > 16384)
    {
        return ids;
    }
    for (const auto &entry : QJsonDocument::fromJson(json.toUtf8()).array())
    {
        const auto id = entry.toString();
        if (validId(id) && !ids.contains(id))
        {
            ids.append(id);
            if (ids.size() == MAX_FAVORITES)
            {
                break;
            }
        }
    }
    return ids;
}

Page parsePage(const QJsonArray &data, const QString &maximumRating)
{
    Page page;
    for (const auto &entry : data)
    {
        const auto obj = entry.toObject();
        const auto rating = obj.value("rating").toString();

        if (!ratingAllowed(rating, maximumRating))
        {
            continue;
        }
        const auto original =
            obj.value("images").toObject().value("original").toObject();
        const int width = original.value("width").toVariant().toInt();
        const int height = original.value("height").toVariant().toInt();
        Gif gif{obj.value("id").toString(),
                obj.value("title").toString().trimmed(),
                original.value("url").toString()};
        if (width > 0 && height > 0 && width <= 16384 && height <= 16384)
        {
            gif.aspectRatio = double(width) / height;
        }
        if (validGif(gif) &&
            std::ranges::none_of(page.gifs, [&](const auto &other) {
                return other.id == gif.id;
            }))
        {
            page.gifs.push_back(std::move(gif));
        }
        if (page.gifs.size() == GIF_PAGE_SIZE)
        {
            break;
        }
    }
    return page;
}

void loadConfig(const QString &channelId, const QString &token,
                const QObject *caller, std::function<void(Config)> success,
                Failure failure)
{
    request(token, R"(query getGifPickerConfig($channelID: ID!) {
        gifPickerConfig(channelID: $channelID) {
            isEnabled isAllowlisted apiKey contentRating
        }
    })",
            {{"channelID", channelId}}, caller)
        .onSuccess([success, failure](const NetworkResult &result) {
            const auto root = responseObject(result);
            const auto config = root.value("data")
                                    .toObject()
                                    .value("gifPickerConfig")
                                    .toObject();
            if (!root.value("errors").toArray().isEmpty() || config.isEmpty())
            {
                failure("Could not load GIFs from Twitch. Check your device "
                        "login and try again.");
                return;
            }
            if (!config.value("isEnabled").toBool() ||
                !config.value("isAllowlisted").toBool())
            {
                failure("GIFs are not available in this channel.");
                return;
            }
            const auto key = config.value("apiKey").toString();
            if (key.isEmpty() || key.size() > 512)
            {
                failure("Twitch did not provide a GIF search key. Try again "
                        "later.");
                return;
            }
            const auto rating = config.value("contentRating").toString();
            success({key, rating == "G_PG"    ? "pg"
                          : rating == "PG_13" ? "pg-13"
                                              : "g"});
        })
        .onError([failure](const NetworkResult &) {
            failure("Could not reach Twitch. Check your connection and device "
                    "login, then try again.");
        })
        .execute();
}

void loadPage(const Config &config, const QString &search,
              const QStringList &ids, int offset, const QObject *caller,
              std::function<void(Page)> success, Failure failure)
{
    QUrl url(ids.isEmpty()
                 ? (search.isEmpty() ? "https://api.giphy.com/v1/gifs/trending"
                                     : "https://api.giphy.com/v1/gifs/search")
                 : "https://api.giphy.com/v1/gifs");
    QUrlQuery query;
    query.addQueryItem("api_key", config.apiKey);
    query.addQueryItem("rating", config.rating);
    query.addQueryItem("limit", QString::number(GIF_PAGE_SIZE));
    query.addQueryItem("offset", QString::number(std::clamp(offset, 0, 4999)));
    if (!search.isEmpty())
    {
        query.addQueryItem("q", search.left(MAX_SEARCH_LENGTH));
    }
    if (!ids.isEmpty())
    {
        query.addQueryItem("ids", ids.mid(offset, GIF_PAGE_SIZE).join(','));
    }
    url.setQuery(query);
    NetworkRequest(url)
        .followRedirects(false)
        .timeout(15000)
        .caller(caller)
        .onSuccess([success, failure, ids, rating = config.rating,
                    offset](const NetworkResult &result) {
            const auto root = result.parseJson();
            if (!root.value("data").isArray() ||
                root.value("meta").toObject().value("status").toInt() != 200)
            {
                failure("Could not load GIFs. Try again.");
                return;
            }
            auto page = parsePage(root.value("data").toArray(), rating);
            if (!ids.isEmpty())
            {
                QVector<Gif> ordered;
                for (const auto &id : ids.mid(offset, GIF_PAGE_SIZE))
                {
                    const auto found =
                        std::ranges::find(page.gifs, id, &Gif::id);

                    ordered.push_back(found != page.gifs.end()
                                          ? *found
                                          : Gif{id, "Unavailable GIF", {}});
                }
                page.gifs = std::move(ordered);
            }
            const auto pagination = root.value("pagination").toObject();
            const int count =
                std::clamp(pagination.value("count").toInt(), 0, GIF_PAGE_SIZE);
            const int next = offset + (ids.isEmpty() ? count : GIF_PAGE_SIZE);
            const int total = ids.isEmpty()
                                  ? pagination.value("total_count").toInt()
                                  : int(ids.size());
            if (next > offset && next < total && next < 5000)
            {
                page.nextOffset = next;
            }
            success(std::move(page));
        })
        .onError([failure](const NetworkResult &) {
            failure("Could not reach Giphy. Try again in a moment.");
        })
        .execute();
}

void send(const QString &channelId, const QString &token, const Gif &gif,
          const QString &search, const QObject *caller,
          std::function<void(SendResult)> callback)
{
    if (!validGif(gif) || channelId.isEmpty() || token.isEmpty())
    {
        callback({false, "This GIF cannot be sent. Search for it again."});
        return;
    }
    QJsonObject input{
        {"channelID", channelId}, {"gifID", gif.id}, {"gifURL", gif.url}};
    if (!search.isEmpty())
    {
        input.insert("searchTerm", search.left(MAX_SEARCH_LENGTH));
    }
    request(token, R"(mutation sendGifMessage($input: SendGifMessageInput!) {
        sendGifMessage(input: $input) {
            error secondsUntilCanSend message { id }
        }
    })",
            {{"input", input}}, caller)
        .onSuccess([callback](const NetworkResult &result) {
            const auto root = responseObject(result);
            const auto obj = root.value("data")
                                 .toObject()
                                 .value("sendGifMessage")
                                 .toObject();
            const auto error = obj.value("error");
            const auto seconds = obj.value("secondsUntilCanSend").toDouble();
            const int cooldown =
                std::isfinite(seconds)
                    ? int(std::ceil(std::clamp(seconds, 0.0, 86400.0)))
                    : 0;
            if (root.value("errors").toArray().isEmpty() && error.isNull() &&
                !obj.value("message")
                     .toObject()
                     .value("id")
                     .toString()
                     .isEmpty())
            {
                callback({true, {}, cooldown});
            }
            else if (!root.value("errors").toArray().isEmpty() ||
                     !error.isString())
            {
                callback({false, "Could not send this GIF. Check your login."});
            }
            else if (cooldown > 0)
            {
                callback({false, "Wait before sending another GIF.", cooldown});
            }
            else if (error.toString() == "TEMPORARILY_UNAVAILABLE")
            {
                callback(
                    {false,
                     "GIFs are temporarily unavailable. Try again later."});
            }
            else
            {
                callback({.rejectionCode = error.toString()});
            }
        })
        .onError([callback](const NetworkResult &) {
            callback({false, "Could not confirm whether the GIF was sent. "
                             "Check chat before trying again."});
        })
        .execute();
}

void loadOwnSubscriptionTier(const QString &channelId, const QString &token,
                             const QObject *caller,
                             std::function<void(int)> success, Failure failure)
{
    request(token, R"(query ownSubscriptionTier($channelID: ID!) {
        user(id: $channelID) {
            self { subscriptionBenefit { tier } }
        }
    })",
            {{"channelID", channelId}}, caller)
        .onSuccess([success, failure](const NetworkResult &result) {
            const auto root = responseObject(result);
            if (!root.value("errors").toArray().isEmpty())
            {
                failure("Twitch didn't say who is logged in");
                return;
            }
            const auto self =
                root.value("data").toObject().value("user").toObject().value(
                    "self");
            if (!self.isObject())
            {
                failure("Twitch didn't say who is logged in");
                return;
            }
            // "1000", "2000", or "3000". Missing when there is no subscription.
            success(self.toObject()
                        .value("subscriptionBenefit")
                        .toObject()
                        .value("tier")
                        .toString()
                        .toInt() /
                    1000);
        })
        .onError([failure](const NetworkResult &) {
            failure("Could not check your subscription.");
        })
        .execute();
}

}  // namespace chatterino::twitchgifs

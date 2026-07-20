// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"
#include "common/enums/UsernameDisplayMode.hpp"
#include "common/network/NetworkRequest.hpp"
#include "providers/twitch/api/HelixEnums.hpp"
#include "providers/twitch/eventsub/SubscriptionRequest.hpp"
#include "providers/twitch/TwitchEmotes.hpp"
#include "util/Helpers.hpp"
#include "util/QStringHash.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>

#include <chrono>
#include <functional>
#include <optional>
#include <unordered_set>
#include <vector>

namespace chatterino {

using HelixFailureCallback = std::function<void()>;
template <typename... T>
using ResultCallback = std::function<void(T...)>;

class CancellationToken;

struct HelixUser {
    QString id;
    QString login;
    QString displayName;
    QString createdAt;
    QString description;
    QString profileImageUrl;

    explicit HelixUser(QJsonObject jsonObject)
        : id(jsonObject.value("id").toString())
        , login(jsonObject.value("login").toString())
        , displayName(jsonObject.value("display_name").toString())
        , createdAt(jsonObject.value("created_at").toString())
        , description(jsonObject.value("description").toString())
        , profileImageUrl(jsonObject.value("profile_image_url").toString())
    {
    }
};

struct HelixMinimalUser {
    QString id;
    QString login;
    QString displayName;

    /// Returns the display name formatted according to @a mode.
    [[nodiscard]] QString formatted(UsernameDisplayMode mode) const
    {
        const bool hasLocalizedName =
            this->displayName.compare(this->login, Qt::CaseInsensitive) != 0;

        switch (mode)
        {
            case UsernameDisplayMode::Username:
                return this->login;

            case UsernameDisplayMode::LocalizedName:
                return hasLocalizedName ? this->displayName : this->login;

            default:
            case UsernameDisplayMode::UsernameAndLocalizedName:
                if (hasLocalizedName)
                {
                    return this->login + QStringLiteral(" (") +
                           this->displayName + QStringLiteral(")");
                }
                return this->login;
        }
    }
};

struct HelixGetChannelFollowersResponse {
    int total;

    explicit HelixGetChannelFollowersResponse(const QJsonObject &jsonObject)
        : total(jsonObject.value("total").toInt())
    {
    }
};

struct HelixStream {
    QString id;
    QString userId;
    QString userLogin;
    QString userName;
    QString gameId;
    QString gameName;
    QString type;
    QString title;
    int viewerCount;
    QString startedAt;
    QString language;
    QString thumbnailUrl;

    std::vector<QString> tags;

    HelixStream()
        : id("")
        , userId("")
        , userLogin("")
        , userName("")
        , gameId("")
        , gameName("")
        , type("")
        , title("")
        , viewerCount()
        , startedAt("")
        , language("")
        , thumbnailUrl("")
    {
    }

    explicit HelixStream(QJsonObject jsonObject)
        : id(jsonObject.value("id").toString())
        , userId(jsonObject.value("user_id").toString())
        , userLogin(jsonObject.value("user_login").toString())
        , userName(jsonObject.value("user_name").toString())
        , gameId(jsonObject.value("game_id").toString())
        , gameName(jsonObject.value("game_name").toString())
        , type(jsonObject.value("type").toString())
        , title(jsonObject.value("title").toString())
        , viewerCount(jsonObject.value("viewer_count").toInt())
        , startedAt(jsonObject.value("started_at").toString())
        , language(jsonObject.value("language").toString())
        , thumbnailUrl(jsonObject.value("thumbnail_url").toString())
    {
        const auto jsonTags = jsonObject.value("tags").toArray();
        for (const auto &tag : jsonTags)
        {
            this->tags.push_back(tag.toString());
        }
    }
};

struct HelixGame {
    QString id;
    QString name;
    QString boxArtUrl;

    explicit HelixGame(QJsonObject jsonObject)
        : id(jsonObject.value("id").toString())
        , name(jsonObject.value("name").toString())
        , boxArtUrl(jsonObject.value("box_art_url").toString())
    {
    }
};

struct HelixClip {
    QString id;
    QString editUrl;

    explicit HelixClip(QJsonObject jsonObject)
        : id(jsonObject.value("id").toString())
        , editUrl(jsonObject.value("edit_url").toString())
    {
    }
};

struct HelixChannel {
    QString userId;
    QString name;
    QString language;
    QString gameId;
    QString gameName;
    QString title;

    explicit HelixChannel(QJsonObject jsonObject)
        : userId(jsonObject.value("broadcaster_id").toString())
        , name(jsonObject.value("broadcaster_name").toString())
        , language(jsonObject.value("broadcaster_language").toString())
        , gameId(jsonObject.value("game_id").toString())
        , gameName(jsonObject.value("game_name").toString())
        , title(jsonObject.value("title").toString())
    {
    }
};

struct HelixStreamMarker {
    QString createdAt;
    QString description;
    QString id;
    int positionSeconds;

    explicit HelixStreamMarker(QJsonObject jsonObject)
        : createdAt(jsonObject.value("created_at").toString())
        , description(jsonObject.value("description").toString())
        , id(jsonObject.value("id").toString())
        , positionSeconds(jsonObject.value("position_seconds").toInt())
    {
    }
};

struct HelixBlock {
    QString userId;
    QString userName;
    QString displayName;

    explicit HelixBlock(QJsonObject jsonObject)
        : userId(jsonObject.value("user_id").toString())
        , userName(jsonObject.value("user_login").toString())
        , displayName(jsonObject.value("display_name").toString())
    {
    }
};

struct HelixCheermoteImage {
    Url imageURL1x;
    Url imageURL2x;
    Url imageURL4x;

    explicit HelixCheermoteImage(QJsonObject jsonObject)
        : imageURL1x(Url{jsonObject.value("1").toString()})
        , imageURL2x(Url{jsonObject.value("2").toString()})
        , imageURL4x(Url{jsonObject.value("4").toString()})
    {
    }
};

struct HelixCheermoteTier {
    QString id;
    QString color;
    int minBits;
    HelixCheermoteImage darkAnimated;
    HelixCheermoteImage darkStatic;
    HelixCheermoteImage lightAnimated;
    HelixCheermoteImage lightStatic;

    explicit HelixCheermoteTier(QJsonObject jsonObject)
        : id(jsonObject.value("id").toString())
        , color(jsonObject.value("color").toString())
        , minBits(jsonObject.value("min_bits").toInt())
        , darkAnimated(jsonObject.value("images")
                           .toObject()
                           .value("dark")
                           .toObject()
                           .value("animated")
                           .toObject())
        , darkStatic(jsonObject.value("images")
                         .toObject()
                         .value("dark")
                         .toObject()
                         .value("static")
                         .toObject())
        , lightAnimated(jsonObject.value("images")
                            .toObject()
                            .value("light")
                            .toObject()
                            .value("animated")
                            .toObject())
        , lightStatic(jsonObject.value("images")
                          .toObject()
                          .value("light")
                          .toObject()
                          .value("static")
                          .toObject())
    {
    }
};

struct HelixCheermoteSet {
    QString prefix;
    QString type;
    std::vector<HelixCheermoteTier> tiers;

    explicit HelixCheermoteSet(QJsonObject jsonObject)
        : prefix(jsonObject.value("prefix").toString())
        , type(jsonObject.value("type").toString())
    {
        for (const auto &tier : jsonObject.value("tiers").toArray())
        {
            this->tiers.emplace_back(tier.toObject());
        }
    }
};

struct HelixEmoteSetData {
    QString setId;
    QString ownerId;
    QString emoteType;

    explicit HelixEmoteSetData(QJsonObject jsonObject)
        : setId(jsonObject.value("emote_set_id").toString())
        , ownerId(jsonObject.value("owner_id").toString())
        , emoteType(jsonObject.value("emote_type").toString())
    {
    }
};

struct HelixChannelEmote {
    const QString id;
    const QString name;
    const QString type;
    const QString setID;
    const QString ownerID;

    explicit HelixChannelEmote(const QJsonObject &jsonObject)
        : id(jsonObject["id"].toString())
        , name(jsonObject["name"].toString())
        , type(jsonObject["emote_type"].toString())
        , setID(jsonObject["emote_set_id"].toString())
        , ownerID(jsonObject["owner_id"].toString())
    {
    }
};

struct HelixChatSettings {
    const QString broadcasterId;
    const bool emoteMode;

    const std::optional<int> followerModeDuration;
    const std::optional<int> nonModeratorChatDelayDuration;
    const std::optional<int> slowModeWaitTime;
    const bool subscriberMode;
    const bool uniqueChatMode;

    explicit HelixChatSettings(QJsonObject jsonObject)
        : broadcasterId(jsonObject.value("broadcaster_id").toString())
        , emoteMode(jsonObject.value("emote_mode").toBool())
        , followerModeDuration(makeConditionedOptional(
              jsonObject.value("follower_mode").toBool(),
              jsonObject.value("follower_mode_duration").toInt()))
        , nonModeratorChatDelayDuration(makeConditionedOptional(
              jsonObject.value("non_moderator_chat_delay").toBool(),
              jsonObject.value("non_moderator_chat_delay_duration").toInt()))
        , slowModeWaitTime(makeConditionedOptional(
              jsonObject.value("slow_mode").toBool(),
              jsonObject.value("slow_mode_wait_time").toInt()))
        , subscriberMode(jsonObject.value("subscriber_mode").toBool())
        , uniqueChatMode(jsonObject.value("unique_chat_mode").toBool())
    {
    }
};

struct HelixVip {
    QString userId;

    QString userName;

    QString userLogin;

    explicit HelixVip(const QJsonObject &jsonObject)
        : userId(jsonObject.value("user_id").toString())
        , userName(jsonObject.value("user_name").toString())
        , userLogin(jsonObject.value("user_login").toString())
    {
    }
};

struct HelixChatters {
    std::unordered_set<QString> chatters;
    int total{};
    QString cursor;

    HelixChatters() = default;

    explicit HelixChatters(const QJsonObject &jsonObject);
};

using HelixModerator = HelixVip;

struct HelixModerators {
    std::vector<HelixModerator> moderators;
    QString cursor;

    HelixModerators() = default;

    explicit HelixModerators(const QJsonObject &jsonObject)
        : cursor(jsonObject.value("pagination")
                     .toObject()
                     .value("cursor")
                     .toString())
    {
        const auto &data = jsonObject.value("data").toArray();
        for (const auto &mod : data)
        {
            HelixModerator moderator(mod.toObject());

            this->moderators.push_back(moderator);
        }
    }
};

struct HelixBadgeVersion {
    QString id;
    Url imageURL1x;
    Url imageURL2x;
    Url imageURL4x;
    QString title;
    Url clickURL;

    explicit HelixBadgeVersion(const QJsonObject &jsonObject)
        : id(jsonObject.value("id").toString())
        , imageURL1x(Url{jsonObject.value("image_url_1x").toString()})
        , imageURL2x(Url{jsonObject.value("image_url_2x").toString()})
        , imageURL4x(Url{jsonObject.value("image_url_4x").toString()})
        , title(jsonObject.value("title").toString())
        , clickURL(Url{jsonObject.value("click_url").toString()})
    {
    }
};

struct HelixBadgeSet {
    QString setID;
    std::vector<HelixBadgeVersion> versions;

    explicit HelixBadgeSet(const QJsonObject &json)
        : setID(json.value("set_id").toString())
    {
        const auto jsonVersions = json.value("versions").toArray();
        for (const auto &version : jsonVersions)
        {
            this->versions.emplace_back(version.toObject());
        }
    }
};

struct HelixGlobalBadges {
    std::vector<HelixBadgeSet> badgeSets;

    explicit HelixGlobalBadges(const QJsonObject &jsonObject)
    {
        const auto &data = jsonObject.value("data").toArray();
        for (const auto &set : data)
        {
            this->badgeSets.emplace_back(set.toObject());
        }
    }
};

using HelixChannelBadges = HelixGlobalBadges;

struct HelixDropReason {
    QString code;
    QString message;

    explicit HelixDropReason(const QJsonObject &jsonObject)
        : code(jsonObject["code"].toString())
        , message(jsonObject["message"].toString())
    {
    }
};

struct HelixSentMessage {
    QString id;
    bool isSent;
    std::optional<HelixDropReason> dropReason;

    explicit HelixSentMessage(const QJsonObject &jsonObject)
        : id(jsonObject["message_id"].toString())
        , isSent(jsonObject["is_sent"].toBool())
        , dropReason(jsonObject.contains("drop_reason")
                         ? std::optional(HelixDropReason(
                               jsonObject["drop_reason"].toObject()))
                         : std::nullopt)
    {
    }
};

struct HelixFollowedChannel {
    QString broadcasterID;
    QString broadcasterLogin;
    QString broadcasterName;
    QDateTime followedAt;

    explicit HelixFollowedChannel(const QJsonObject &jsonObject)
        : broadcasterID(jsonObject["broadcaster_id"].toString())
        , broadcasterLogin(jsonObject["broadcaster_login"].toString())
        , broadcasterName(jsonObject["broadcaster_name"].toString())
        , followedAt([&jsonObject] {
            const auto followedAtString = jsonObject["followed_at"].toString();
            auto timestamp =
                QDateTime::fromString(followedAtString, Qt::ISODateWithMs);
            if (!timestamp.isValid())
            {
                timestamp =
                    QDateTime::fromString(followedAtString, Qt::ISODate);
            }
            return timestamp;
        }())
    {
    }
};

struct HelixSendMessageArgs {
    QString broadcasterID;
    QString senderID;
    QString message;

    QString replyParentMessageID;
};

struct HelixPollChoice {
    QString id;
    QString title;
    int votes;

    explicit HelixPollChoice(const QJsonObject &jsonObject)
        : id(jsonObject.value("id").toString())
        , title(jsonObject.value("title").toString())
        , votes(jsonObject.value("votes").toInt())
    {
    }
};

struct HelixPoll {
    QString id;
    QString title;
    std::vector<HelixPollChoice> choices;
    QString status;

    explicit HelixPoll(const QJsonObject &jsonObject)
        : id(jsonObject.value("id").toString())
        , title(jsonObject.value("title").toString())
        , status(jsonObject.value("status").toString())
    {
        const auto &data = jsonObject.value("choices").toArray();
        this->choices.reserve(data.size());
        for (const auto &c : data)
        {
            HelixPollChoice choice(c.toObject());
            this->choices.push_back(choice);
        }
    }
};

struct HelixPolls {
    std::vector<HelixPoll> polls;

    HelixPolls() = default;

    explicit HelixPolls(const QJsonObject &jsonObject)
    {
        const auto &data = jsonObject.value("data").toArray();
        this->polls.reserve(data.size());
        for (const auto &p : data)
        {
            HelixPoll poll(p.toObject());
            this->polls.push_back(poll);
        }
    }
};

inline QString helixPredictionOutcomeIdFromJson(const QJsonValue &v)
{
    if (v.isString())
    {
        return v.toString();
    }
    if (!v.isUndefined() && !v.isNull())
    {
        return v.toVariant().toString();
    }
    return {};
}

struct HelixPredictionOutcome {
    QString id;
    QString title;
    int users;
    int channelPoints;
    /// GQL `PredictionOutcomeColor` string, e.g. BLUE, PINK (may be empty).
    QString color;

    explicit HelixPredictionOutcome(const QJsonObject &jsonObject)
        : id(helixPredictionOutcomeIdFromJson(
              jsonObject.value(QStringLiteral("id"))))
        , title(jsonObject.value("title").toString())
        , users(jsonObject.value("users").toInt())
        , channelPoints(jsonObject.value("channel_points").toInt())
        , color(jsonObject.value(QStringLiteral("color")).toString())
    {
    }
};

struct HelixPrediction {
    QString id;
    QString title;
    QString winningOutcomeID;
    /// From GQL `winningOutcome.title` when present; helps match winner if ids differ.
    QString winningOutcomeTitle;
    QString status;
    std::vector<HelixPredictionOutcome> outcomes;
    QString createdAt;
    /// When the event was resolved or canceled (GQL `endedAt`); empty if not ended.
    QString endedAt;
    int predictionWindow{0};
    /// From GQL `self.prediction.outcome.id` when the logged-in user has a prediction.
    QString viewerPredictionOutcomeId;
    /// Channel points the user already spent on this prediction (GQL `self.prediction.points`).
    int viewerPredictionPoints{0};

    explicit HelixPrediction(const QJsonObject &jsonObject)
        : id(jsonObject.value("id").toString())
        , title(jsonObject.value("title").toString())
        , winningOutcomeID(helixPredictionOutcomeIdFromJson(
              jsonObject.value(QStringLiteral("winning_outcome_id"))))
        , winningOutcomeTitle(
              jsonObject.value(QStringLiteral("winning_outcome_title"))
                  .toString())
        , status(jsonObject.value("status").toString())
        , createdAt(jsonObject.value("created_at").toString())
        , endedAt(jsonObject.value(QStringLiteral("ended_at")).toString())
        , predictionWindow(jsonObject.value("prediction_window").toInt())
        , viewerPredictionOutcomeId(helixPredictionOutcomeIdFromJson(
              jsonObject.value(QStringLiteral("viewer_prediction_outcome_id"))))
        , viewerPredictionPoints(
              jsonObject.value(QStringLiteral("viewer_prediction_points"))
                  .toInt())
    {
        const auto &data = jsonObject.value("outcomes").toArray();
        this->outcomes.reserve(data.size());
        for (const auto &o : data)
        {
            HelixPredictionOutcome outcome(o.toObject());
            this->outcomes.push_back(outcome);
        }
    }
};

struct HelixPredictions {
    std::vector<HelixPrediction> predictions;

    HelixPredictions() = default;

    explicit HelixPredictions(const QJsonObject &jsonObject)
    {
        const auto &data = jsonObject.value("data").toArray();
        this->predictions.reserve(data.size());
        for (const auto &p : data)
        {
            HelixPrediction prediction(p.toObject());
            this->predictions.push_back(prediction);
        }
    }
};

struct HelixSharedChatSession {
    QStringList participantIds;

    explicit HelixSharedChatSession(const QJsonObject &jsonObject)
    {
        const auto &participants = jsonObject.value("participants").toArray();
        for (const auto p : participants)
        {
            const auto broadcasterId =
                p.toObject().value("broadcaster_id").toString();
            this->participantIds.push_back(broadcasterId);
        }
    }
};

struct HelixStartCommercialResponse {
    int length;

    QString message;

    int retryAfter;

    explicit HelixStartCommercialResponse(const QJsonObject &jsonObject)
    {
        auto jsonData = jsonObject.value("data").toArray().at(0).toObject();
        this->length = jsonData.value("length").toInt();
        this->message = jsonData.value("message").toString();
        this->retryAfter = jsonData.value("retry_after").toInt();
    }
};

struct HelixShieldModeStatus {
    bool isActive;

    QString moderatorID;

    QString moderatorLogin;

    QString moderatorName;

    QDateTime lastActivatedAt;

    explicit HelixShieldModeStatus(const QJsonObject &json)
        : isActive(json["is_active"].toBool())
        , moderatorID(json["moderator_id"].toString())
        , moderatorLogin(json["moderator_login"].toString())
        , moderatorName(json["moderator_name"].toString())
        , lastActivatedAt(QDateTime::fromString(
              json["last_activated_at"].toString(), Qt::ISODate))
    {
        this->lastActivatedAt.setTimeZone(QTimeZone::utc());
    }
};

struct HelixError {
    QString error;

    int status;

    QString message;

    explicit HelixError(const QJsonObject &json)
        : error(json["error"].toString())
        , status(json["status"].toInt())
        , message(json["message"].toString())
    {
    }
};

using HelixGetChannelBadgesError = HelixGetGlobalBadgesError;

struct HelixPaginationState {
    bool done;
};

struct HelixCreateEventSubSubscriptionResponse {
    QString subscriptionID;
    QString subscriptionStatus;
    QString subscriptionType;
    QString subscriptionVersion;
    QJsonObject subscriptionCondition;
    QString subscriptionCreatedAt;
    QString subscriptionSessionID;
    QString subscriptionConnectedAt;
    int subscriptionCost;

    int total;
    int totalCost;
    int maxTotalCost;

    explicit HelixCreateEventSubSubscriptionResponse(
        const QJsonObject &jsonObject)
    {
        {
            auto jsonData = jsonObject.value("data").toArray().at(0).toObject();
            this->subscriptionID = jsonData.value("id").toString();
            this->subscriptionStatus = jsonData.value("status").toString();
            this->subscriptionType = jsonData.value("type").toString();
            this->subscriptionVersion = jsonData.value("version").toString();
            this->subscriptionCondition =
                jsonData.value("condition").toObject();
            this->subscriptionCreatedAt =
                jsonData.value("created_at").toString();
            this->subscriptionSessionID = jsonData.value("transport")
                                              .toObject()
                                              .value("session_id")
                                              .toString();
            this->subscriptionConnectedAt = jsonData.value("transport")
                                                .toObject()
                                                .value("connected_at")
                                                .toString();
            this->subscriptionCost = jsonData.value("cost").toInt();
        }

        this->total = jsonObject.value("total").toInt();
        this->totalCost = jsonObject.value("total_cost").toInt();
        this->maxTotalCost = jsonObject.value("max_total_cost").toInt();
    }

    friend QDebug &operator<<(
        QDebug &dbg, const HelixCreateEventSubSubscriptionResponse &data);
};

struct HelixPinnedChatMessage {
    HelixMinimalUser sender;
    HelixMinimalUser pinnedBy;
    QString messageID;
    QString messageText;
    QDateTime startsAt;
    std::optional<QDateTime> endsAt;

    explicit HelixPinnedChatMessage(const QJsonObject &data)
        : sender({
              .id = data["sender_user_id"].toString(),
              .login = data["sender_user_login"].toString(),
              .displayName = data["sender_user_name"].toString(),
          })
        , pinnedBy({
              .id = data["pinned_by_user_id"].toString(),
              .login = data["pinned_by_user_login"].toString(),
              .displayName = data["pinned_by_user_name"].toString(),
          })
        , messageID(data["message_id"].toString())
        , messageText(data["message"].toObject().value("text").toString())
        , startsAt(
              QDateTime::fromString(data["starts_at"].toString(), Qt::ISODate))
    {
        auto endsAt = data["ends_at"].toString();
        if (!endsAt.isEmpty())
        {
            this->endsAt = QDateTime::fromString(endsAt, Qt::ISODate);
        }
    }
};

class IHelix
{
public:
    template <typename... T>
    using FailureCallback = std::function<void(T...)>;

    virtual void fetchUsers(
        QStringList userIds, QStringList userLogins,
        ResultCallback<std::vector<HelixUser>> successCallback,
        HelixFailureCallback failureCallback) = 0;
    virtual void getUserByName(QString userName,
                               ResultCallback<HelixUser> successCallback,
                               HelixFailureCallback failureCallback) = 0;
    virtual void getUserById(QString userId,
                             ResultCallback<HelixUser> successCallback,
                             HelixFailureCallback failureCallback) = 0;

    virtual void getChannelFollowers(
        QString broadcasterID,
        ResultCallback<HelixGetChannelFollowersResponse> successCallback,
        std::function<void(QString)> failureCallback) = 0;

    virtual void fetchStreams(
        QStringList userIds, QStringList userLogins,
        ResultCallback<std::vector<HelixStream>> successCallback,
        HelixFailureCallback failureCallback,
        std::function<void()> finallyCallback) = 0;

    virtual void getStreamById(
        QString userId, ResultCallback<bool, HelixStream> successCallback,
        HelixFailureCallback failureCallback,
        std::function<void()> finallyCallback) = 0;

    virtual void getStreamByName(
        QString userName, ResultCallback<bool, HelixStream> successCallback,
        HelixFailureCallback failureCallback,
        std::function<void()> finallyCallback) = 0;

    virtual void fetchGames(
        QStringList gameIds, QStringList gameNames,
        ResultCallback<std::vector<HelixGame>> successCallback,
        HelixFailureCallback failureCallback) = 0;

    virtual void searchGames(
        QString gameName,
        ResultCallback<std::vector<HelixGame>> successCallback,
        HelixFailureCallback failureCallback) = 0;

    virtual void getGameById(QString gameId,
                             ResultCallback<HelixGame> successCallback,
                             HelixFailureCallback failureCallback) = 0;

    virtual void createClip(
        QString channelId, QString title, std::optional<int> duration,
        ResultCallback<HelixClip> successCallback,
        std::function<void(HelixClipError, QString)> failureCallback,
        std::function<void()> finallyCallback) = 0;

    virtual void fetchChannels(
        QStringList userIDs,
        ResultCallback<std::vector<HelixChannel>> successCallback,
        HelixFailureCallback failureCallback) = 0;

    virtual void getChannel(QString broadcasterId,
                            ResultCallback<HelixChannel> successCallback,
                            HelixFailureCallback failureCallback) = 0;

    virtual void createStreamMarker(
        QString broadcasterId, QString description,
        ResultCallback<HelixStreamMarker> successCallback,
        std::function<void(HelixStreamMarkerError)> failureCallback) = 0;

    virtual void loadBlocks(
        QString userId, ResultCallback<std::vector<HelixBlock>> pageCallback,
        FailureCallback<QString> failureCallback,
        CancellationToken &&token) = 0;

    virtual void blockUser(QString targetUserId, const QObject *caller,
                           std::function<void()> successCallback,
                           HelixFailureCallback failureCallback) = 0;

    virtual void unblockUser(QString targetUserId, const QObject *caller,
                             std::function<void()> successCallback,
                             HelixFailureCallback failureCallback) = 0;

    virtual void updateChannel(
        QString broadcasterId, QString gameId, QString language, QString title,
        std::function<void(NetworkResult)> successCallback,
        FailureCallback<HelixUpdateChannelError, QString> failureCallback) = 0;

    virtual void manageAutoModMessages(
        QString userID, QString msgID, QString action,
        std::function<void()> successCallback,
        std::function<void(HelixAutoModMessageError)> failureCallback) = 0;

    virtual void getCheermotes(
        QString broadcasterId,
        ResultCallback<std::vector<HelixCheermoteSet>> successCallback,
        HelixFailureCallback failureCallback) = 0;

    virtual void getEmoteSetData(
        QString emoteSetId, ResultCallback<HelixEmoteSetData> successCallback,
        HelixFailureCallback failureCallback) = 0;

    virtual void getChannelEmotes(
        QString broadcasterId,
        ResultCallback<std::vector<HelixChannelEmote>> successCallback,
        HelixFailureCallback failureCallback) = 0;

    virtual void updateUserChatColor(
        QString userID, QString color, ResultCallback<> successCallback,
        FailureCallback<HelixUpdateUserChatColorError, QString>
            failureCallback) = 0;

    virtual void deleteChatMessages(
        QString broadcasterID, QString moderatorID, QString messageID,
        ResultCallback<> successCallback,
        FailureCallback<HelixDeleteChatMessagesError, QString>
            failureCallback) = 0;

    virtual void addChannelModerator(
        QString broadcasterID, QString userID, ResultCallback<> successCallback,
        FailureCallback<HelixAddChannelModeratorError, QString>
            failureCallback) = 0;

    virtual void removeChannelModerator(
        QString broadcasterID, QString userID, ResultCallback<> successCallback,
        FailureCallback<HelixRemoveChannelModeratorError, QString>
            failureCallback) = 0;

    virtual void sendChatAnnouncement(
        QString broadcasterID, QString moderatorID, QString message,
        HelixAnnouncementColor color, ResultCallback<> successCallback,
        FailureCallback<HelixSendChatAnnouncementError, QString>
            failureCallback) = 0;

    virtual void addChannelVIP(
        QString broadcasterID, QString userID, ResultCallback<> successCallback,
        FailureCallback<HelixAddChannelVIPError, QString> failureCallback) = 0;

    virtual void removeChannelVIP(
        QString broadcasterID, QString userID, ResultCallback<> successCallback,
        FailureCallback<HelixRemoveChannelVIPError, QString>
            failureCallback) = 0;

    virtual void unbanUser(
        QString broadcasterID, QString moderatorID, QString userID,
        ResultCallback<> successCallback,
        FailureCallback<HelixUnbanUserError, QString> failureCallback) = 0;

    virtual void startRaid(
        QString fromBroadcasterID, QString toBroadcasterID,
        ResultCallback<> successCallback,
        FailureCallback<HelixStartRaidError, QString> failureCallback) = 0;

    virtual void cancelRaid(
        QString broadcasterID, ResultCallback<> successCallback,
        FailureCallback<HelixCancelRaidError, QString> failureCallback) = 0;

    virtual void updateEmoteMode(
        QString broadcasterID, QString moderatorID, bool emoteMode,
        ResultCallback<HelixChatSettings> successCallback,
        FailureCallback<HelixUpdateChatSettingsError, QString>
            failureCallback) = 0;

    virtual void updateFollowerMode(
        QString broadcasterID, QString moderatorID,
        std::optional<int> followerModeDuration,
        ResultCallback<HelixChatSettings> successCallback,
        FailureCallback<HelixUpdateChatSettingsError, QString>
            failureCallback) = 0;

    virtual void updateNonModeratorChatDelay(
        QString broadcasterID, QString moderatorID,
        std::optional<int> nonModeratorChatDelayDuration,
        ResultCallback<HelixChatSettings> successCallback,
        FailureCallback<HelixUpdateChatSettingsError, QString>
            failureCallback) = 0;

    virtual void updateSlowMode(
        QString broadcasterID, QString moderatorID,
        std::optional<int> slowModeWaitTime,
        ResultCallback<HelixChatSettings> successCallback,
        FailureCallback<HelixUpdateChatSettingsError, QString>
            failureCallback) = 0;

    virtual void updateSubscriberMode(
        QString broadcasterID, QString moderatorID, bool subscriberMode,
        ResultCallback<HelixChatSettings> successCallback,
        FailureCallback<HelixUpdateChatSettingsError, QString>
            failureCallback) = 0;

    virtual void updateUniqueChatMode(
        QString broadcasterID, QString moderatorID, bool uniqueChatMode,
        ResultCallback<HelixChatSettings> successCallback,
        FailureCallback<HelixUpdateChatSettingsError, QString>
            failureCallback) = 0;

    virtual void banUser(
        QString broadcasterID, QString moderatorID, QString userID,
        std::optional<int> duration, QString reason,
        ResultCallback<> successCallback,
        FailureCallback<HelixBanUserError, QString> failureCallback) = 0;

    virtual void warnUser(
        QString broadcasterID, QString moderatorID, QString userID,
        QString reason, ResultCallback<> successCallback,
        FailureCallback<HelixWarnUserError, QString> failureCallback) = 0;

    virtual void addSuspiciousUser(
        QString broadcasterID, QString moderatorID, QString userID,
        bool restricted, ResultCallback<> successCallback,
        FailureCallback<QString> failureCallback) = 0;

    virtual void removeSuspiciousUser(
        QString broadcasterID, QString moderatorID, QString userID,
        ResultCallback<> successCallback,
        FailureCallback<QString> failureCallback) = 0;

    virtual void sendWhisper(
        QString fromUserID, QString toUserID, QString message,
        ResultCallback<> successCallback,
        FailureCallback<HelixWhisperError, QString> failureCallback) = 0;

    virtual void getChatters(
        QString broadcasterID, QString moderatorID, size_t maxChattersToFetch,
        ResultCallback<HelixChatters> successCallback,
        FailureCallback<HelixGetChattersError, QString> failureCallback) = 0;

    virtual void getModerators(
        QString broadcasterID, int maxModeratorsToFetch,
        ResultCallback<std::vector<HelixModerator>> successCallback,
        FailureCallback<HelixGetModeratorsError, QString> failureCallback) = 0;

    virtual void getChannelVIPs(
        QString broadcasterID,
        ResultCallback<std::vector<HelixVip>> successCallback,
        FailureCallback<HelixListVIPsError, QString> failureCallback) = 0;

    virtual void startCommercial(
        QString broadcasterID, int length,
        ResultCallback<HelixStartCommercialResponse> successCallback,
        FailureCallback<HelixStartCommercialError, QString>
            failureCallback) = 0;

    virtual void getGlobalBadges(
        ResultCallback<HelixGlobalBadges> successCallback,
        FailureCallback<HelixGetGlobalBadgesError, QString>
            failureCallback) = 0;

    virtual void getChannelBadges(
        QString broadcasterID,
        ResultCallback<HelixChannelBadges> successCallback,
        FailureCallback<HelixGetChannelBadgesError, QString>
            failureCallback) = 0;

    virtual void updateShieldMode(
        QString broadcasterID, QString moderatorID, bool isActive,
        ResultCallback<HelixShieldModeStatus> successCallback,
        FailureCallback<HelixUpdateShieldModeError, QString>
            failureCallback) = 0;

    virtual void sendShoutout(
        QString fromBroadcasterID, QString toBroadcasterID, QString moderatorID,
        ResultCallback<> successCallback,
        FailureCallback<HelixSendShoutoutError, QString> failureCallback) = 0;

    virtual void sendChatMessage(
        HelixSendMessageArgs args,
        ResultCallback<HelixSentMessage> successCallback,
        FailureCallback<HelixSendMessageError, QString> failureCallback) = 0;

    virtual void getUserEmotes(
        QString userID, QString broadcasterID,
        ResultCallback<std::vector<HelixChannelEmote>, HelixPaginationState>
            pageCallback,
        FailureCallback<QString> failureCallback,
        CancellationToken &&token) = 0;

    virtual void getFollowedChannel(
        QString userID, QString broadcasterID, const QObject *caller,
        ResultCallback<std::optional<HelixFollowedChannel>> successCallback,
        FailureCallback<QString> failureCallback) = 0;

    virtual void createPoll(QString broadcasterID, QString title,
                            QStringList choices, std::chrono::seconds duration,
                            int pointsPerVote, ResultCallback<> successCallback,
                            FailureCallback<QString> failureCallback) = 0;

    virtual void getPolls(QString broadcasterID, QStringList ids, int first,
                          QString after,
                          ResultCallback<HelixPolls> successCallback,
                          FailureCallback<QString> failureCallback) = 0;

    virtual void endPoll(QString broadcasterID, QString id,
                         bool immediatelyHide,
                         ResultCallback<HelixPoll> successCallback,
                         FailureCallback<QString> failureCallback) = 0;

    virtual void createPrediction(QString broadcasterID, QString title,
                                  QStringList choices,
                                  std::chrono::seconds duration,
                                  ResultCallback<> successCallback,
                                  FailureCallback<QString> failureCallback) = 0;

    virtual void getPredictions(
        QString broadcasterID, QStringList ids, int first, QString after,
        ResultCallback<HelixPredictions> successCallback,
        FailureCallback<QString> failureCallback) = 0;

    virtual void endPrediction(QString broadcasterID, QString id,
                               bool refundPoints, QString winningOutcomeID,
                               ResultCallback<HelixPrediction> successCallback,
                               FailureCallback<QString> failureCallback) = 0;

    virtual void createEventSubSubscription(
        const eventsub::SubscriptionRequest &request, const QString &sessionID,
        ResultCallback<HelixCreateEventSubSubscriptionResponse> successCallback,
        FailureCallback<HelixCreateEventSubSubscriptionError, QString>
            failureCallback,
        const QString &clientIdOverride = {},
        const QString &oauthTokenOverride = {}) = 0;

    virtual void deleteEventSubSubscription(
        const QString &subscriptionID, ResultCallback<> successCallback,
        FailureCallback<QString> failureCallback) = 0;

    // https://dev.twitch.tv/docs/api/reference#pin-chat-message
    virtual void pinChatMessage(
        const QString &broadcasterID, const QString &moderatorID,
        const QString &messageID, std::optional<std::chrono::seconds> duration,
        ResultCallback<> successCallback,
        FailureCallback<HelixPinMessageError, QString> failureCallback) = 0;

    // https://dev.twitch.tv/docs/api/reference/#update-pinned-chat-message
    virtual void updatePinnedChatMessage(
        const QString &broadcasterID, const QString &moderatorID,
        const QString &messageID, std::optional<std::chrono::seconds> duration,
        ResultCallback<> successCallback,
        FailureCallback<HelixPinMessageError, QString> failureCallback) = 0;

    // https://dev.twitch.tv/docs/api/reference/#get-pinned-chat-message
    virtual void getPinnedChatMessage(
        const QString &broadcasterID, const QString &moderatorID,
        ResultCallback<std::optional<HelixPinnedChatMessage>> successCallback,
        FailureCallback<QString> failureCallback) = 0;

    // https://dev.twitch.tv/docs/api/reference/#unpin-chat-message
    virtual void unpinChatMessage(
        const QString &broadcasterID, const QString &moderatorID,
        const QString &messageID, ResultCallback<> successCallback,
        FailureCallback<HelixUnpinMessageError, QString> failureCallback) = 0;

    // https://dev.twitch.tv/docs/api/reference/#get-shared-chat-session
    virtual void getSharedChatSession(
        QString broadcasterID,
        ResultCallback<HelixSharedChatSession> successCallback,
        FailureCallback<HelixGetSharedChatSessionError, QString>
            failureCallback) = 0;

    virtual void update(QString clientId, QString oauthToken) = 0;

protected:
    virtual void updateChatSettings(
        QString broadcasterID, QString moderatorID, QJsonObject json,
        ResultCallback<HelixChatSettings> successCallback,
        FailureCallback<HelixUpdateChatSettingsError, QString>
            failureCallback) = 0;
};

class Helix final : public IHelix
{
public:
    void fetchUsers(QStringList userIds, QStringList userLogins,
                    ResultCallback<std::vector<HelixUser>> successCallback,
                    HelixFailureCallback failureCallback) final;
    void getUserByName(QString userName,
                       ResultCallback<HelixUser> successCallback,
                       HelixFailureCallback failureCallback) final;
    void getUserById(QString userId, ResultCallback<HelixUser> successCallback,
                     HelixFailureCallback failureCallback) final;

    void getChannelFollowers(
        QString broadcasterID,
        ResultCallback<HelixGetChannelFollowersResponse> successCallback,
        std::function<void(QString)> failureCallback) final;

    void fetchStreams(QStringList userIds, QStringList userLogins,
                      ResultCallback<std::vector<HelixStream>> successCallback,
                      HelixFailureCallback failureCallback,
                      std::function<void()> finallyCallback) final;

    void getStreamById(QString userId,
                       ResultCallback<bool, HelixStream> successCallback,
                       HelixFailureCallback failureCallback,
                       std::function<void()> finallyCallback) final;

    void getStreamByName(QString userName,
                         ResultCallback<bool, HelixStream> successCallback,
                         HelixFailureCallback failureCallback,
                         std::function<void()> finallyCallback) final;

    void fetchGames(QStringList gameIds, QStringList gameNames,
                    ResultCallback<std::vector<HelixGame>> successCallback,
                    HelixFailureCallback failureCallback) final;

    void searchGames(QString gameName,
                     ResultCallback<std::vector<HelixGame>> successCallback,
                     HelixFailureCallback failureCallback) final;

    void getGameById(QString gameId, ResultCallback<HelixGame> successCallback,
                     HelixFailureCallback failureCallback) final;

    void createClip(
        QString channelId, QString title, std::optional<int> duration,
        ResultCallback<HelixClip> successCallback,
        std::function<void(HelixClipError, QString)> failureCallback,
        std::function<void()> finallyCallback) final;

    void fetchChannels(
        QStringList userIDs,
        ResultCallback<std::vector<HelixChannel>> successCallback,
        HelixFailureCallback failureCallback) final;

    void getChannel(QString broadcasterId,
                    ResultCallback<HelixChannel> successCallback,
                    HelixFailureCallback failureCallback) final;

    void createStreamMarker(
        QString broadcasterId, QString description,
        ResultCallback<HelixStreamMarker> successCallback,
        std::function<void(HelixStreamMarkerError)> failureCallback) final;

    void loadBlocks(QString userId,
                    ResultCallback<std::vector<HelixBlock>> pageCallback,
                    FailureCallback<QString> failureCallback,
                    CancellationToken &&token) final;

    void blockUser(QString targetUserId, const QObject *caller,
                   std::function<void()> successCallback,
                   HelixFailureCallback failureCallback) final;

    void unblockUser(QString targetUserId, const QObject *caller,
                     std::function<void()> successCallback,
                     HelixFailureCallback failureCallback) final;

    void updateChannel(QString broadcasterId, QString gameId, QString language,
                       QString title,
                       std::function<void(NetworkResult)> successCallback,
                       FailureCallback<HelixUpdateChannelError, QString>
                           failureCallback) final;

    void manageAutoModMessages(
        QString userID, QString msgID, QString action,
        std::function<void()> successCallback,
        std::function<void(HelixAutoModMessageError)> failureCallback) final;

    void getCheermotes(
        QString broadcasterId,
        ResultCallback<std::vector<HelixCheermoteSet>> successCallback,
        HelixFailureCallback failureCallback) final;

    void getEmoteSetData(QString emoteSetId,
                         ResultCallback<HelixEmoteSetData> successCallback,
                         HelixFailureCallback failureCallback) final;

    void getChannelEmotes(
        QString broadcasterId,
        ResultCallback<std::vector<HelixChannelEmote>> successCallback,
        HelixFailureCallback failureCallback) final;

    void updateUserChatColor(
        QString userID, QString color, ResultCallback<> successCallback,
        FailureCallback<HelixUpdateUserChatColorError, QString> failureCallback)
        final;

    void deleteChatMessages(
        QString broadcasterID, QString moderatorID, QString messageID,
        ResultCallback<> successCallback,
        FailureCallback<HelixDeleteChatMessagesError, QString> failureCallback)
        final;

    void addChannelModerator(
        QString broadcasterID, QString userID, ResultCallback<> successCallback,
        FailureCallback<HelixAddChannelModeratorError, QString> failureCallback)
        final;

    void removeChannelModerator(
        QString broadcasterID, QString userID, ResultCallback<> successCallback,
        FailureCallback<HelixRemoveChannelModeratorError, QString>
            failureCallback) final;

    void sendChatAnnouncement(
        QString broadcasterID, QString moderatorID, QString message,
        HelixAnnouncementColor color, ResultCallback<> successCallback,
        FailureCallback<HelixSendChatAnnouncementError, QString>
            failureCallback) final;

    void addChannelVIP(QString broadcasterID, QString userID,
                       ResultCallback<> successCallback,
                       FailureCallback<HelixAddChannelVIPError, QString>
                           failureCallback) final;

    void removeChannelVIP(QString broadcasterID, QString userID,
                          ResultCallback<> successCallback,
                          FailureCallback<HelixRemoveChannelVIPError, QString>
                              failureCallback) final;

    void unbanUser(
        QString broadcasterID, QString moderatorID, QString userID,
        ResultCallback<> successCallback,
        FailureCallback<HelixUnbanUserError, QString> failureCallback) final;

    void startRaid(
        QString fromBroadcasterID, QString toBroadcasterID,
        ResultCallback<> successCallback,
        FailureCallback<HelixStartRaidError, QString> failureCallback) final;

    void cancelRaid(
        QString broadcasterID, ResultCallback<> successCallback,
        FailureCallback<HelixCancelRaidError, QString> failureCallback) final;

    void updateEmoteMode(QString broadcasterID, QString moderatorID,
                         bool emoteMode,
                         ResultCallback<HelixChatSettings> successCallback,
                         FailureCallback<HelixUpdateChatSettingsError, QString>
                             failureCallback) final;

    void updateFollowerMode(
        QString broadcasterID, QString moderatorID,
        std::optional<int> followerModeDuration,
        ResultCallback<HelixChatSettings> successCallback,
        FailureCallback<HelixUpdateChatSettingsError, QString> failureCallback)
        final;

    void updateNonModeratorChatDelay(
        QString broadcasterID, QString moderatorID,
        std::optional<int> nonModeratorChatDelayDuration,
        ResultCallback<HelixChatSettings> successCallback,
        FailureCallback<HelixUpdateChatSettingsError, QString> failureCallback)
        final;

    void updateSlowMode(QString broadcasterID, QString moderatorID,
                        std::optional<int> slowModeWaitTime,
                        ResultCallback<HelixChatSettings> successCallback,
                        FailureCallback<HelixUpdateChatSettingsError, QString>
                            failureCallback) final;

    void updateSubscriberMode(
        QString broadcasterID, QString moderatorID, bool subscriberMode,
        ResultCallback<HelixChatSettings> successCallback,
        FailureCallback<HelixUpdateChatSettingsError, QString> failureCallback)
        final;

    void updateUniqueChatMode(
        QString broadcasterID, QString moderatorID, bool uniqueChatMode,
        ResultCallback<HelixChatSettings> successCallback,
        FailureCallback<HelixUpdateChatSettingsError, QString> failureCallback)
        final;

    void banUser(
        QString broadcasterID, QString moderatorID, QString userID,
        std::optional<int> duration, QString reason,
        ResultCallback<> successCallback,
        FailureCallback<HelixBanUserError, QString> failureCallback) final;

    void warnUser(
        QString broadcasterID, QString moderatorID, QString userID,
        QString reason, ResultCallback<> successCallback,
        FailureCallback<HelixWarnUserError, QString> failureCallback) final;

    void addSuspiciousUser(QString broadcasterID, QString moderatorID,
                           QString userID, bool restricted,
                           ResultCallback<> successCallback,
                           FailureCallback<QString> failureCallback) final;

    void removeSuspiciousUser(QString broadcasterID, QString moderatorID,
                              QString userID, ResultCallback<> successCallback,
                              FailureCallback<QString> failureCallback) final;

    void sendWhisper(
        QString fromUserID, QString toUserID, QString message,
        ResultCallback<> successCallback,
        FailureCallback<HelixWhisperError, QString> failureCallback) final;

    void getChatters(
        QString broadcasterID, QString moderatorID, size_t maxChattersToFetch,
        ResultCallback<HelixChatters> successCallback,
        FailureCallback<HelixGetChattersError, QString> failureCallback) final;

    void getModerators(
        QString broadcasterID, int maxModeratorsToFetch,
        ResultCallback<std::vector<HelixModerator>> successCallback,
        FailureCallback<HelixGetModeratorsError, QString> failureCallback)
        final;

    void getChannelVIPs(
        QString broadcasterID,
        ResultCallback<std::vector<HelixVip>> successCallback,
        FailureCallback<HelixListVIPsError, QString> failureCallback) final;

    void startCommercial(
        QString broadcasterID, int length,
        ResultCallback<HelixStartCommercialResponse> successCallback,
        FailureCallback<HelixStartCommercialError, QString> failureCallback)
        final;

    void getGlobalBadges(ResultCallback<HelixGlobalBadges> successCallback,
                         FailureCallback<HelixGetGlobalBadgesError, QString>
                             failureCallback) final;

    void getChannelBadges(QString broadcasterID,
                          ResultCallback<HelixChannelBadges> successCallback,
                          FailureCallback<HelixGetChannelBadgesError, QString>
                              failureCallback) final;

    void updateShieldMode(QString broadcasterID, QString moderatorID,
                          bool isActive,
                          ResultCallback<HelixShieldModeStatus> successCallback,
                          FailureCallback<HelixUpdateShieldModeError, QString>
                              failureCallback) final;

    void sendShoutout(
        QString fromBroadcasterID, QString toBroadcasterID, QString moderatorID,
        ResultCallback<> successCallback,
        FailureCallback<HelixSendShoutoutError, QString> failureCallback) final;

    void sendChatMessage(
        HelixSendMessageArgs args,
        ResultCallback<HelixSentMessage> successCallback,
        FailureCallback<HelixSendMessageError, QString> failureCallback) final;

    void getUserEmotes(
        QString userID, QString broadcasterID,
        ResultCallback<std::vector<HelixChannelEmote>, HelixPaginationState>
            pageCallback,
        FailureCallback<QString> failureCallback,
        CancellationToken &&token) final;

    void getFollowedChannel(
        QString userID, QString broadcasterID, const QObject *caller,
        ResultCallback<std::optional<HelixFollowedChannel>> successCallback,
        FailureCallback<QString> failureCallback) final;

    void createPoll(QString broadcasterID, QString title, QStringList choices,
                    std::chrono::seconds duration, int pointsPerVote,
                    ResultCallback<> successCallback,
                    FailureCallback<QString> failureCallback) final;

    void getPolls(QString broadcasterID, QStringList ids, int first,
                  QString after, ResultCallback<HelixPolls> successCallback,
                  FailureCallback<QString> failureCallback) final;

    void endPoll(QString broadcasterID, QString id, bool immediatelyHide,
                 ResultCallback<HelixPoll> successCallback,
                 FailureCallback<QString> failureCallback) final;

    void createPrediction(QString broadcasterID, QString title,
                          QStringList outcomes, std::chrono::seconds duration,
                          ResultCallback<> successCallback,
                          FailureCallback<QString> failureCallback) final;

    void getPredictions(QString broadcasterID, QStringList ids, int first,
                        QString after,
                        ResultCallback<HelixPredictions> successCallback,
                        FailureCallback<QString> failureCallback) final;

    void endPrediction(QString broadcasterID, QString id, bool refundPoints,
                       QString winningOutcomeID,
                       ResultCallback<HelixPrediction> successCallback,
                       FailureCallback<QString> failureCallback) final;

    void createEventSubSubscription(
        const eventsub::SubscriptionRequest &request, const QString &sessionID,
        ResultCallback<HelixCreateEventSubSubscriptionResponse> successCallback,
        FailureCallback<HelixCreateEventSubSubscriptionError, QString>
            failureCallback,
        const QString &clientIdOverride = {},
        const QString &oauthTokenOverride = {}) final;

    void deleteEventSubSubscription(
        const QString &subscriptionID, ResultCallback<> successCallback,
        FailureCallback<QString> failureCallback) final;

    // https://dev.twitch.tv/docs/api/reference/#pin-chat-message
    void pinChatMessage(
        const QString &broadcasterID, const QString &moderatorID,
        const QString &messageID, std::optional<std::chrono::seconds> duration,
        ResultCallback<> successCallback,
        FailureCallback<HelixPinMessageError, QString> failureCallback) final;

    // https://dev.twitch.tv/docs/api/reference/#update-pinned-chat-message
    void updatePinnedChatMessage(
        const QString &broadcasterID, const QString &moderatorID,
        const QString &messageID, std::optional<std::chrono::seconds> duration,
        ResultCallback<> successCallback,
        FailureCallback<HelixPinMessageError, QString> failureCallback) final;

    // https://dev.twitch.tv/docs/api/reference/#get-pinned-chat-message
    void getPinnedChatMessage(
        const QString &broadcasterID, const QString &moderatorID,
        ResultCallback<std::optional<HelixPinnedChatMessage>> successCallback,
        FailureCallback<QString> failureCallback) final;

    // https://dev.twitch.tv/docs/api/reference/#unpin-chat-message
    void unpinChatMessage(
        const QString &broadcasterID, const QString &moderatorID,
        const QString &messageID, ResultCallback<> successCallback,
        FailureCallback<HelixUnpinMessageError, QString> failureCallback) final;

    // https://dev.twitch.tv/docs/api/reference/#get-shared-chat-session
    void getSharedChatSession(
        QString broadcasterID,
        ResultCallback<HelixSharedChatSession> successCallback,
        FailureCallback<HelixGetSharedChatSessionError, QString>
            failureCallback) final;

    void update(QString clientId, QString oauthToken) final;

    static void initialize();

protected:
    void updateChatSettings(
        QString broadcasterID, QString moderatorID, QJsonObject json,
        ResultCallback<HelixChatSettings> successCallback,
        FailureCallback<HelixUpdateChatSettingsError, QString> failureCallback)
        final;

    void onFetchChattersSuccess(
        std::shared_ptr<HelixChatters> finalChatters, QString broadcasterID,
        QString moderatorID, size_t maxChattersToFetch,
        ResultCallback<HelixChatters> successCallback,
        FailureCallback<HelixGetChattersError, QString> failureCallback,
        HelixChatters chatters);

    void fetchChatters(
        QString broadcasterID, QString moderatorID, int first, QString after,
        ResultCallback<HelixChatters> successCallback,
        FailureCallback<HelixGetChattersError, QString> failureCallback);

    void onFetchModeratorsSuccess(
        std::shared_ptr<std::vector<HelixModerator>> finalModerators,
        QString broadcasterID, size_t maxModeratorsToFetch,
        ResultCallback<std::vector<HelixModerator>> successCallback,
        FailureCallback<HelixGetModeratorsError, QString> failureCallback,
        HelixModerators moderators);

    void fetchModerators(
        QString broadcasterID, int first, QString after,
        ResultCallback<HelixModerators> successCallback,
        FailureCallback<HelixGetModeratorsError, QString> failureCallback);

private:
    NetworkRequest makeRequest(const QString &url, const QUrlQuery &urlQuery,
                               NetworkRequestType type);
    NetworkRequest makeRequest(const QString &url, const QUrlQuery &urlQuery,
                               NetworkRequestType type, const QString &clientId,
                               const QString &oauthToken);
    NetworkRequest makeGet(const QString &url, const QUrlQuery &urlQuery);
    NetworkRequest makeDelete(const QString &url, const QUrlQuery &urlQuery);
    NetworkRequest makePost(const QString &url, const QUrlQuery &urlQuery);
    NetworkRequest makePost(const QString &url, const QUrlQuery &urlQuery,
                            const QString &clientId, const QString &oauthToken);
    NetworkRequest makePut(const QString &url, const QUrlQuery &urlQuery);
    NetworkRequest makePatch(const QString &url, const QUrlQuery &urlQuery);

    void paginate(const QString &url, const QUrlQuery &baseQuery,
                  std::function<bool(const QJsonObject &,
                                     const HelixPaginationState &state)>
                      onPage,
                  std::function<void(NetworkResult)> onError,
                  CancellationToken &&token);

    QString clientId;
    QString oauthToken;
};

void initializeHelix(IHelix *_instance);

IHelix *getHelix();

}  // namespace chatterino

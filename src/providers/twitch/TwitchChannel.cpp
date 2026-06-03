// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/TwitchChannel.hpp"

#include "Application.hpp"
#include "common/Common.hpp"
#include "common/Env.hpp"
#include "common/Literals.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "controllers/notifications/NotificationController.hpp"
#include "controllers/twitch/LiveController.hpp"
#include "debug/AssertInGuiThread.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/Link.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "messages/MessageThread.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/bttv/BttvLiveUpdates.hpp"
#include "providers/bttv/liveupdates/BttvLiveUpdateMessages.hpp"
#include "providers/emoji/Emojis.hpp"
#include "providers/ffz/FfzBadges.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/folhinha/FolhinhaBadges.hpp"
#include "providers/homies/HomiesBadges.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/moltorino/MoltorinoSupporterBadges.hpp"
#include "providers/recentmessages/Api.hpp"
#include "providers/seventv/eventapi/Dispatch.hpp"
#include "providers/seventv/SeventvAPI.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvEventAPI.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/ChannelPointReward.hpp"
#include "providers/twitch/eventsub/Controller.hpp"
#include "providers/twitch/IrcMessageHandler.hpp"
#include "providers/twitch/PubSubManager.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/twitch/TwitchUsers.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/Toasts.hpp"
#include "singletons/WindowManager.hpp"
#include "util/FormatTime.hpp"
#include "util/Helpers.hpp"
#include "util/PostToThread.hpp"
#include "util/QStringHash.hpp"
#include "util/VectorMessageSink.hpp"
#include "widgets/Window.hpp"

#include <IrcConnection>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocale>
#include <QStringBuilder>
#include <QThread>
#include <QTimer>
#include <rapidjson/document.h>

#include <algorithm>
#include <memory>

namespace chatterino {

using namespace literals;
using namespace std::chrono_literals;

namespace detail {

bool isUnknownCommand(const QString &text)
{
    static QRegularExpression isUnknownCommand(
        R"(^(?:\.(?!\.|$)|\/)(?!me(?:\s|$)|\s))",
        QRegularExpression::CaseInsensitiveOption);

    auto match = isUnknownCommand.match(text);

    return match.hasMatch();
}

}  // namespace detail

using detail::isUnknownCommand;

namespace {
const QString MAGIC_MESSAGE_SUFFIX = u" \u034f"_s;
constexpr int CLIP_CREATION_COOLDOWN = 5000;
constexpr qint64 CHANNEL_POINTS_MIN_REFRESH_INTERVAL_MS = 10'000;
constexpr qint64 CHANNEL_POINTS_STALE_AFTER_MS = 120'000;
constexpr qint64 PREDICTION_MIN_REFRESH_INTERVAL_MS = 10'000;
constexpr qint64 PREDICTION_STALE_AFTER_MS = 120'000;
constexpr qint64 POLL_MIN_REFRESH_INTERVAL_MS = 10'000;
constexpr qint64 POLL_STALE_AFTER_MS = 120'000;
constexpr qint64 CHAT_WARNING_MIN_REFRESH_INTERVAL_MS = 15'000;
constexpr qint64 CHAT_WARNING_STALE_AFTER_MS = 120'000;
constexpr qint64 CHAT_WARNING_AUTH_PROMPT_COOLDOWN_MS = 30'000;
constexpr qint64 LEAD_MOD_RETRY_INTERVAL_MS = 30'000;
constexpr qint64 FOLLOWING_STATUS_RETRY_INTERVAL_MS = 30'000;
constexpr qint64 LOCALLY_CLEARED_RAID_SUPPRESSION_MS = 120'000;
const QString CLIPS_LINK("https://clips.twitch.tv/%1");
const QString CLIPS_FAILURE_CLIPS_UNAVAILABLE_TEXT(
    "Failed to create a clip - clips are temporarily unavailable: %1");

QString duplicateBypassSuffix(int nonce)
{
    const auto tagCodePoint =
        static_cast<char32_t>(0xE0000 + std::clamp(nonce, 1, 0x7F));
    return MAGIC_MESSAGE_SUFFIX + QString::fromUcs4(&tagCodePoint, 1);
}

QString helixSendMessageErrorText(HelixSendMessageError error, QString message)
{
    if (message.isEmpty())
    {
        message = "(empty message)";
    }

    switch (error)
    {
        case HelixSendMessageError::MissingText:
            return "You can't send an empty message.";
        case HelixSendMessageError::BadRequest:
            return "Failed to send message: " + message;
        case HelixSendMessageError::Forbidden:
            return "You are not allowed to send messages in this channel.";
        case HelixSendMessageError::MessageTooLarge:
            return "Your message was too long.";
        case HelixSendMessageError::UserMissingScope:
            return "Missing required scope. Re-login with your account and "
                   "try again.";
        case HelixSendMessageError::Forwarded:
            return message;
        case HelixSendMessageError::Unknown:
        default:
            return "Unknown error: " + message;
    }
}
const QString CLIPS_FAILURE_CLIPS_DISABLED_TEXT(
    "Failed to create a clip - the streamer has clips disabled in their "
    "channel.");
const QString CLIPS_FAILURE_CLIPS_RESTRICTED_TEXT(
    "Failed to create a clip - the streamer has restricted clip creation "
    "to subscribers, or followers of an unknown duration.");
const QString CLIPS_FAILURE_CLIPS_RESTRICTED_CATEGORY_TEXT(
    "Failed to create a clip - the streamer has disabled clips while in "
    "this category.");
const QString CLIPS_FAILURE_NOT_AUTHENTICATED_TEXT(
    "Failed to create a clip - you need to re-authenticate.");
const QString CLIPS_FAILURE_UNKNOWN_ERROR_TEXT("Failed to create a clip: %1");
const QString LOGIN_PROMPT_TEXT("Click here to add your account again.");
const Link ACCOUNTS_LINK(Link::OpenAccountsPage, QString());

struct BotBadgeSendConfig {
    QString appToken;
    QString clientId;
    QString senderID;

    bool isValid() const
    {
        return !this->appToken.isEmpty() && !this->clientId.isEmpty() &&
               !this->senderID.isEmpty();
    }
};

BotBadgeSendConfig getBotBadgeSendConfig()
{
    const auto &settings = *getSettings();

    return {
        settings.botBadgeAppAccessToken.getValue().trimmed(),
        settings.botBadgeClientID.getValue().trimmed(),
        settings.botBadgeUserID.getValue().trimmed(),
    };
}

bool shouldUseBotBadgeForSelectedAccount()
{
    const auto &settings = *getSettings();
    if (!settings.botBadgeAlwaysUse)
    {
        return false;
    }

    if (settings.botBadgeOverrideAllAccounts)
    {
        return true;
    }

    const auto botUserID = settings.botBadgeUserID.getValue().trimmed();
    const auto currentAccount = getApp()->getAccounts()->twitch.getCurrent();
    return currentAccount != nullptr && !botUserID.isEmpty() &&
           currentAccount->getUserId() == botUserID;
}

bool selectedAccountIsBotBadgeSender(const QString &senderID)
{
    const auto currentAccount = getApp()->getAccounts()->twitch.getCurrent();
    return currentAccount != nullptr && !senderID.isEmpty() &&
           currentAccount->getUserId() == senderID;
}

bool authTokenBelongsToChannel(const MoltorinoAuthToken &auth,
                               const TwitchChannel &channel)
{
    const auto userId = auth.userId.trimmed();
    if (!userId.isEmpty() && userId == channel.roomId())
    {
        return true;
    }

    const auto login = auth.login.trimmed();
    return !login.isEmpty() &&
           login.compare(channel.getName(), Qt::CaseInsensitive) == 0;
}

bool authTokenHasKnownIdentity(const MoltorinoAuthToken &auth)
{
    const auto login = auth.login.trimmed();
    return !auth.userId.trimmed().isEmpty() ||
           (!login.isEmpty() &&
            login.compare("legacy token", Qt::CaseInsensitive) != 0);
}

QString normalizeMoltorinoAuthError(const QString &action, const QString &error)
{
    return MoltorinoAuth::normalizeAuthError(action, error);
}

QString getUserPointsChannelId(const QJsonObject &payload)
{
    const auto dataValue = payload.value("data");
    if (!dataValue.isObject())
    {
        return {};
    }

    const auto data = dataValue.toObject();
    const auto balanceValue = data.value("balance");
    if (!balanceValue.isObject())
    {
        return {};
    }

    const auto balanceObj = balanceValue.toObject();
    auto channelId = balanceObj.value("channel_id").toString();
    if (channelId.isEmpty())
    {
        channelId = data.value("channel_id").toString();
    }

    return channelId;
}

QDateTime parseIsoDateTime(const QJsonValue &value)
{
    if (!value.isString())
    {
        return {};
    }

    auto dt = QDateTime::fromString(value.toString(), Qt::ISODate);
    if (!dt.isValid())
    {
        dt = QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
    }
    return dt;
}

qint64 parseJsonInteger(const QJsonValue &value)
{
    if (value.isDouble())
    {
        return qint64(value.toDouble());
    }
    if (value.isString())
    {
        bool ok = false;
        const auto parsed = value.toString().trimmed().toLongLong(&ok);
        return ok ? parsed : 0;
    }
    return 0;
}

QString userDisplayNameFromObject(const QJsonObject &obj)
{
    auto name = obj.value("displayName").toString();
    if (name.isEmpty())
    {
        name = obj.value("display_name").toString();
    }
    if (name.isEmpty())
    {
        name = obj.value("user_display_name").toString();
    }
    if (name.isEmpty())
    {
        name = obj.value("login").toString();
    }
    if (name.isEmpty())
    {
        name = obj.value("user_login").toString();
    }
    if (name.isEmpty())
    {
        name = obj.value("user_name").toString();
    }
    if (name.isEmpty())
    {
        name = obj.value("name").toString();
    }
    return name;
}

QJsonObject objectFromAnyKey(const QJsonObject &obj, const QString &snakeKey,
                             const QString &camelKey)
{
    auto value = obj.value(snakeKey).toObject();
    if (value.isEmpty())
    {
        value = obj.value(camelKey).toObject();
    }
    return value;
}

QString predictionActorOrFallback(const QString &actor)
{
    return actor.trimmed().isEmpty() ? QStringLiteral("Twitch") : actor;
}

QString pinnedChatEventPinId(const QJsonObject &data)
{
    auto id = data.value("id").toString();
    if (id.isEmpty())
    {
        id = data.value("pin_id").toString();
    }
    if (id.isEmpty())
    {
        id = data.value("pinned_chat_message_id").toString();
    }
    if (id.isEmpty())
    {
        id = data.value("pinnedChatMessageId").toString();
    }
    if (id.isEmpty())
    {
        id = data.value("pinnedChatMessageID").toString();
    }

    return id;
}

QString predictionWinnerTitle(const TwitchChannel::PredictionEvent &prediction)
{
    if (prediction.winningOutcomeId.isEmpty())
    {
        return {};
    }

    for (const auto &outcome : prediction.outcomes)
    {
        if (outcome.id == prediction.winningOutcomeId)
        {
            return outcome.title;
        }
    }

    return {};
}

QString predictionSystemMessageKind(
    const QString &type, const TwitchChannel::PredictionEvent &prediction)
{
    if (type == "event-created")
    {
        return QStringLiteral("created");
    }
    if (type == "event-locked" ||
        (type == "event-updated" &&
         prediction.status.compare("LOCKED", Qt::CaseInsensitive) == 0))
    {
        return QStringLiteral("locked");
    }
    if (type == "event-canceled" ||
        (type == "event-updated" &&
         prediction.status.compare("CANCELED", Qt::CaseInsensitive) == 0))
    {
        return QStringLiteral("canceled");
    }
    if (type == "event-resolved" ||
        (type == "event-updated" &&
         prediction.status.compare("RESOLVED", Qt::CaseInsensitive) == 0))
    {
        return QStringLiteral("resolved");
    }

    return {};
}

QString predictionSystemMessageKey(
    const QString &kind, const TwitchChannel::PredictionEvent &prediction)
{
    if (kind.isEmpty())
    {
        return {};
    }

    auto subject = prediction.id;
    if (subject.isEmpty())
    {
        subject = prediction.title;
    }

    return subject + ':' + kind + ':' + prediction.winningOutcomeId;
}

QString sanitizeChatWarningReason(QString reason)
{
    reason.replace(QChar(0x034F), QString());
    reason = reason.simplified();
    if (reason.isEmpty())
    {
        return QStringLiteral("No moderator message was provided");
    }
    return reason;
}

QString chatWarningMessageId(const TwitchChannel::ChatWarning &warning)
{
    auto id = warning.id;
    if (id.isEmpty())
    {
        id = warning.channelId % u"_" %
             QString::number(warning.createdAt.toMSecsSinceEpoch());
    }

    return u"moltorino_chat_warning_" % warning.channelId % u"_" % id;
}

bool isWarningAcknowledgeNotice(const QString &text)
{
    return text.startsWith(
               "You received a Warning from a moderator in this channel.",
               Qt::CaseInsensitive) ||
           text.contains("Acknowledge the Warning at", Qt::CaseInsensitive);
}

QString chatWarningAuthFallbackText(const QString &channelName)
{
    return QString("You have an active warning in this channel. "
                   "Authenticate in Settings -> Moltorino -> Authentication to "
                   "view and acknowledge it in Leafyrino, or open "
                   "https://www.twitch.tv/%1 in your browser.")
        .arg(channelName);
}

QString chatWarningMissingDetailsText(const QString &channelName)
{
    return QString(
               "Your message was blocked because of an active warning, but "
               "Leafyrino could not load the warning details yet. Try again in "
               "a moment, or open https://www.twitch.tv/%1 in your browser to "
               "acknowledge it.")
        .arg(channelName);
}

bool isChannelCurrentlyVisible(const TwitchChannel &channel)
{
    const auto visibleChannels =
        getApp()->getWindows()->getVisibleChannelNames();
    for (const auto &visible : visibleChannels)
    {
        if (visible.compare(channel.getName(), Qt::CaseInsensitive) == 0)
        {
            return true;
        }
    }
    return false;
}

MessagePtr makeChatWarningMessage(const TwitchChannel &channel,
                                  const TwitchChannel::ChatWarning &warning)
{
    const auto reason = sanitizeChatWarningReason(warning.reason);
    const auto text =
        QStringLiteral("Leafyrino: You received a warning: \"%1\" Acknowledge")
            .arg(reason);
    const auto timestamp = warning.createdAt.isValid()
                               ? warning.createdAt.toLocalTime().time()
                               : QTime::currentTime();

    MessageBuilder builder;
    builder.message().id = chatWarningMessageId(warning);
    builder.message().channelName = channel.getName();
    builder.message().loginName = QStringLiteral("leafyrino");
    builder.message().displayName = QStringLiteral("Leafyrino");
    builder.message().localizedName = QStringLiteral("Leafyrino");
    builder.message().usernameColor = QColor("#FFA500");
    builder.message().serverReceivedTime =
        warning.createdAt.isValid() ? warning.createdAt
                                    : QDateTime::currentDateTimeUtc();
    builder.message().messageText = text;
    builder.message().searchText = text;
    builder.message().flags.set(MessageFlag::System);
    builder.message().flags.set(MessageFlag::PubSub);
    builder.message().flags.set(MessageFlag::ChatWarning);
    builder.message().flags.set(MessageFlag::DoNotTriggerNotification);

    builder.emplace<TimestampElement>(timestamp);
    builder.emplace<TextElement>("Leafyrino:", MessageElementFlag::Text,
                                 MessageColor(QColor("#FFA500")),
                                 FontStyle::ChatMediumBold);
    builder.emplace<TextElement>("You received a warning:",
                                 MessageElementFlag::Text, MessageColor::Text);
    builder.emplace<TextElement>(u"\"" % reason % u"\"",
                                 MessageElementFlag::Text, MessageColor::Text,
                                 FontStyle::ChatMediumBold);
    builder.emplace<TextElement>("-", MessageElementFlag::Text,
                                 MessageColor::Text);
    auto *acknowledge = builder.emplace<TextElement>(
        "Acknowledge", MessageElementFlag::Text,
        MessageColor(QColor("#00C853")), FontStyle::ChatMediumBold);
    acknowledge->setLink({Link::AcknowledgeChatWarning, warning.channelId});
    acknowledge->setTooltip("Acknowledge this warning");

    return builder.release();
}

// Maximum number of chatters to fetch when refreshing chatters
constexpr auto MAX_CHATTERS_TO_FETCH = 5000;

// From Twitch docs - expected size for a badge (1x)
constexpr QSize BASE_BADGE_SIZE(18, 18);

}  // namespace

TwitchChannel::TwitchChannel(const QString &name, bool anonymous)
    : Channel(name, Channel::Type::Twitch)
    , ChannelChatters(*static_cast<Channel *>(this))
    , nameOptions{name, name, name}
    , anonymous_(anonymous)
    , subscriptionUrl_("https://www.twitch.tv/subs/" + name)
    , channelUrl_("https://www.twitch.tv/" + name)
    , popoutPlayerUrl_(TWITCH_PLAYER_URL.arg(name))
    , localTwitchEmotes_(std::make_shared<EmoteMap>())
    , bttvEmotes_(std::make_shared<EmoteMap>())
    , ffzEmotes_(std::make_shared<EmoteMap>())
    , seventvEmotes_(std::make_shared<EmoteMap>())
{
    qCDebug(chatterinoTwitch) << "[TwitchChannel" << name << "] Opened";

    auto clearPersonalAuthState = [this](bool clearFollowingStatus) {
        this->setLeadMod(false, false);
        this->leadModFetchInFlight_.store(false);
        this->leadModLookupAttempted_ = false;
        this->lastLeadModRefreshAt_ = QDateTime();
        if (clearFollowingStatus)
        {
            this->followingStatusFetchInFlight_.store(false);
            this->followingStatusUserId_.clear();
            this->lastFollowingStatusRefreshAt_ = QDateTime();
            const bool hadFollowingStatus =
                this->followingStatusKnown_ || this->following_;
            this->following_ = false;
            this->followingStatusKnown_ = false;
            this->followedAt_.reset();
            if (hadFollowingStatus)
            {
                this->followingStatusChanged.invoke();
            }
        }
        this->channelPointsFetchInFlight_.store(false);
        this->lastChannelPointsRefreshAt_ = QDateTime();
        this->lastChannelPointsUpdateAt_ = QDateTime();
        const bool hadPointsError = !this->lastChannelPointsError_.isEmpty();
        this->lastChannelPointsError_.clear();
        if (this->channelPoints_.exchange(-1) != -1 || hadPointsError)
        {
            this->channelPointsChanged.invoke();
        }
    };

    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->twitch.currentUserAboutToChange,
        [this](const auto &oldAccount, const auto & /*newAccount*/) {
            if (oldAccount && !oldAccount->isAnon() &&
                !oldAccount->getUserId().isEmpty())
            {
                getApp()->getTwitchPubSub()->forgetUserAuthenticatedTopics(
                    oldAccount->getUserId());
            }
            this->eventSubChannelChatUserMessageHoldHandle.reset();
            this->eventSubChannelChatUserMessageUpdateHandle.reset();
            this->eventSubChannelModerateHandle.reset();
            this->eventSubAutomodMessageHoldHandle.reset();
            this->eventSubAutomodMessageUpdateHandle.reset();
            this->eventSubSuspiciousUserMessageHandle.reset();
            this->eventSubSuspiciousUserUpdateHandle.reset();
        });

    this->bSignals_.emplace_back(
        getApp()->getAccounts()->twitch.currentUserChanged.connect(
            [this, clearPersonalAuthState] {
                this->setMod(false);
                clearPersonalAuthState(true);
                this->refreshPubSub();
                this->refreshTwitchChannelEmotes(false);

                auto account = getApp()->getAccounts()->twitch.getCurrent();
                if (account && !account->isAnon() && !this->roomId().isEmpty())
                {
                    this->refreshBadges();
                    auto shared = std::dynamic_pointer_cast<TwitchChannel>(
                        this->weak_from_this().lock());
                    if (shared)
                    {
                        getApp()->getTwitchLiveController()->add(shared);
                    }
                }
            }));
    getSettings()->customPinAuthToken.connect(
        [this](const QString &, auto) {
            this->refreshPubSub();
        },
        this->signalHolder_);
    getSettings()->moltorinoAuthAccounts.connect(
        [this, clearPersonalAuthState](const QString &, auto) {
            clearPersonalAuthState(false);
            this->refreshPubSub();
        },
        this->signalHolder_);
    getSettings()->showRaidStatusAboveInput.connect(
        [this](const auto &, auto) {
            this->refreshPubSub();
        },
        this->signalHolder_);

    this->refreshPubSub();
    // We can safely ignore this signal connection since it's a private signal, meaning
    // it will only ever be invoked by TwitchChannel itself
    std::ignore = this->userStateChanged.connect([this] {
        this->refreshPubSub();
    });

    // We can safely ignore this signal connection this has no external dependencies - once the signal
    // is destroyed, it will no longer be able to fire
    std::ignore = this->joined.connect([this]() {
        if (this->disconnected_)
        {
            this->loadRecentMessagesReconnect();
            this->lastConnectedAt_ = std::chrono::system_clock::now();
            this->disconnected_ = false;
        }
    });

    // timers
    QObject::connect(&this->chattersListTimer_, &QTimer::timeout, [this] {
        this->refreshChatters();
    });

    this->chattersListTimer_.start(5 * 60 * 1000);

    QObject::connect(&this->threadClearTimer_, &QTimer::timeout, [this] {
        // We periodically check for any dangling reply threads that missed
        // being cleaned up on messageRemovedFromStart. This could occur if
        // some other part of the program, like a user card, held a reference
        // to the message.
        //
        // It seems difficult to actually replicate a situation where things
        // are actually cleaned up, but I've verified that cleanups DO happen.
        this->cleanUpReplyThreads();
    });
    this->threadClearTimer_.start(5 * 60 * 1000);

    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->twitch.emotesReloaded,
        [this](auto *caller, const auto &result) {
            if (result)
            {
                // emotes were reloaded - clear follower emotes if the user is
                // now subscribed to the streamer
                if (!this->localTwitchEmotes_.get()->empty() &&
                    getApp()->getAccounts()->twitch.getCurrent()->hasEmoteSet(
                        EmoteSetId{this->localTwitchEmoteSetID_.get()}))
                {
                    this->localTwitchEmotes_.set(std::make_shared<EmoteMap>());
                }

                if (caller == this)
                {
                    this->addSystemMessage(
                        "Twitch subscriber emotes reloaded.");
                }
                return;
            }

            if (caller == this || caller == nullptr)
            {
                this->addSystemMessage(
                    u"Failed to load Twitch subscriber emotes: " %
                    result.error());
            }
        });

    this->signalHolder_.managedConnect(
        getApp()->getTwitchPubSub()->poll.updated, [this](const auto &d) {
            if (!d["topic"].toString().endsWith(this->roomId()))
            {
                return;
            }

            const auto payload = d;
            const auto weak = this->weak_from_this();
            runInGuiThread([this, weak, payload] {
                if (auto shared = weak.lock())
                {
                    this->handlePollUpdate(payload);
                }
            });
        });

    this->signalHolder_.managedConnect(
        getApp()->getTwitchPubSub()->raid.updated, [this](const auto &d) {
            const auto expectedTopic =
                QStringLiteral("raid.%1").arg(this->roomId());
            if (d["topic"].toString() != expectedTopic)
            {
                return;
            }

            const auto payload = d;
            const auto weak = this->weak_from_this();
            runInGuiThread([weak, payload] {
                if (auto shared =
                        std::dynamic_pointer_cast<TwitchChannel>(weak.lock()))
                {
                    shared->handleRaidUpdate(payload);
                }
            });
        });

    this->signalHolder_.managedConnect(
        getApp()->getTwitchPubSub()->userPoints.updated, [this](const auto &d) {
            auto account = getApp()->getAccounts()->twitch.getCurrent();
            if (account)
            {
                const auto auth = MoltorinoAuth::resolveCurrentUserToken();
                const auto expectedUserId =
                    auth.userId.isEmpty() ? account->getUserId() : auth.userId;
                if (expectedUserId.isEmpty() ||
                    !d["topic"].toString().endsWith(expectedUserId))
                {
                    return;
                }

                const auto channelId = getUserPointsChannelId(d);
                if (channelId.isEmpty() || channelId != this->roomId())
                {
                    return;
                }

                const auto payload = d;
                const auto weak = this->weak_from_this();
                runInGuiThread([this, weak, payload] {
                    if (auto shared = weak.lock())
                    {
                        this->handleUserPointsUpdate(payload);
                    }
                });
            }
        });

    QObject::connect(&this->sendWaitTimer_, &QTimer::timeout,
                     &this->lifetimeGuard_, [this] {
                         this->syncSendWaitTimer();
                     });

    // debugging
#if 0
    for (int i = 0; i < 1000; i++) {
        // this->addSystemMessage("asef");
    }
#endif
}

TwitchChannel::~TwitchChannel()
{
    if (isAppAboutToQuit())
    {
        return;
    }

    getApp()->getTwitch()->dropSeventvChannel(this->seventvUserID_,
                                              this->seventvEmoteSetID_);

    if (getApp()->getBttvLiveUpdates())
    {
        getApp()->getBttvLiveUpdates()->partChannel(this->roomId());
    }

    if (getApp()->getSeventvEventAPI())
    {
        getApp()->getSeventvEventAPI()->unsubscribeTwitchChannel(
            this->roomId());
    }

    this->destroyed.invoke();
}

void TwitchChannel::initialize()
{
    this->refreshBadges();
}

bool TwitchChannel::isEmpty() const
{
    return this->getName().isEmpty();
}

bool TwitchChannel::canSendMessage() const
{
    return !this->isEmpty() && !this->anonymous_;
}

bool TwitchChannel::isAnonymous() const
{
    return this->anonymous_;
}

const QString &TwitchChannel::getDisplayName() const
{
    return this->nameOptions.displayName;
}

void TwitchChannel::setDisplayName(const QString &name)
{
    this->nameOptions.displayName = name;
}

const QString &TwitchChannel::getLocalizedName() const
{
    return this->nameOptions.localizedName;
}

void TwitchChannel::setLocalizedName(const QString &name)
{
    this->nameOptions.localizedName = name;
}

void TwitchChannel::refreshTwitchChannelEmotes(bool manualRefresh)
{
    if (getApp()->isTest())
    {
        return;
    }

    if (manualRefresh)
    {
        getApp()->getAccounts()->twitch.getCurrent()->reloadEmotes(this);
    }

    // Twitch's 'Get User Emotes' doesn't assigns a different set-ID to follower
    // emotes compared to subscriber emotes.
    QString setID = TWITCH_SUB_EMOTE_SET_PREFIX % this->roomId();
    this->localTwitchEmoteSetID_.set(setID);
    if (getApp()->getAccounts()->twitch.getCurrent()->hasEmoteSet(
            EmoteSetId{setID}))
    {
        this->localTwitchEmotes_.set(std::make_shared<EmoteMap>());
        if (getSettings()->showFollowButtonInSplitHeader)
        {
            this->refreshFollowingStatus(false);
        }
        return;
    }

    auto makeEmotes = [](const auto &emotes) {
        EmoteMap map;
        for (const auto &emote : emotes)
        {
            if (emote.type != u"follower")
            {
                continue;
            }
            map.emplace(
                EmoteName{emote.name},
                getApp()->getEmotes()->getTwitchEmotes()->getOrCreateEmote(
                    EmoteId{emote.id}, EmoteName{emote.name}));
        }
        return map;
    };

    const auto requestUserId =
        getApp()->getAccounts()->twitch.getCurrent()->getUserId();
    const auto requestRoomId = this->roomId();

    getHelix()->getFollowedChannel(
        requestUserId, requestRoomId, nullptr,
        [weak{this->weak_from_this()}, makeEmotes, requestUserId,
         requestRoomId](const auto &chan) {
            auto self = std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
            if (!self)
            {
                return;
            }

            auto current = getApp()->getAccounts()->twitch.getCurrent();
            const auto currentUserId = current && !current->isAnon()
                                           ? current->getUserId()
                                           : QString();
            if (currentUserId != requestUserId ||
                self->roomId() != requestRoomId)
            {
                return;
            }

            self->setFollowingStatus(
                chan.has_value(),
                chan ? std::optional<QDateTime>(chan->followedAt)
                     : std::nullopt);
            if (!chan)
            {
                return;
            }
            getHelix()->getChannelEmotes(
                self->roomId(),
                [weak, makeEmotes](const auto &emotes) {
                    auto self =
                        std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
                    if (!self)
                    {
                        return;
                    }

                    self->localTwitchEmotes_.set(
                        std::make_shared<EmoteMap>(makeEmotes(emotes)));
                },
                [weak] {
                    auto self = weak.lock();
                    if (!self)
                    {
                        return;
                    }
                    self->addSystemMessage("Failed to load follower emotes.");
                });
        },
        [](const auto &error) {
            qCWarning(chatterinoTwitch)
                << "Failed to get following status:" << error;
        });
}

void TwitchChannel::refreshBTTVChannelEmotes(bool manualRefresh)
{
    if (!Settings::instance().enableBTTVChannelEmotes)
    {
        this->bttvEmotes_.set(EMPTY_EMOTE_MAP);
        return;
    }

    bool cacheHit = readProviderEmotesCache(
        this->roomId(), "betterttv",
        [this, weak = weakOf<Channel>(this)](auto jsonDoc) {
            if (auto shared = weak.lock())
            {
                auto emoteMap = bttv::detail::parseChannelEmotes(
                    jsonDoc.object(), this->getLocalizedName());
                this->setBttvEmotes(std::make_shared<const EmoteMap>(emoteMap));
            }
        });

    BttvEmotes::loadChannel(
        weakOf<Channel>(this), this->roomId(), this->getLocalizedName(),
        [this, weak = weakOf<Channel>(this)](auto &&emoteMap) {
            if (auto shared = weak.lock())
            {
                this->setBttvEmotes(std::make_shared<const EmoteMap>(emoteMap));
            }
        },
        manualRefresh, cacheHit);
}

void TwitchChannel::refreshFFZChannelEmotes(bool manualRefresh)
{
    if (!Settings::instance().enableFFZChannelEmotes)
    {
        this->ffzEmotes_.set(EMPTY_EMOTE_MAP);
        return;
    }

    bool cacheHit = readProviderEmotesCache(
        this->roomId(), "frankerfacez", [this](const auto &jsonDoc) {
            auto emoteMap = ffz::detail::parseChannelEmotes(jsonDoc.object());
            this->setFfzEmotes(std::make_shared<const EmoteMap>(emoteMap));
        });

    FfzEmotes::loadChannel(
        weakOf<Channel>(this), this->roomId(),
        [this, weak = weakOf<Channel>(this)](auto &&emoteMap) {
            if (auto shared = weak.lock())
            {
                this->setFfzEmotes(std::make_shared<const EmoteMap>(emoteMap));
            }
        },
        [this, weak = weakOf<Channel>(this)](auto &&modBadge) {
            if (auto shared = weak.lock())
            {
                this->ffzCustomModBadge_.set(
                    std::forward<decltype(modBadge)>(modBadge));
            }
        },
        [this, weak = weakOf<Channel>(this)](auto &&vipBadge) {
            if (auto shared = weak.lock())
            {
                this->ffzCustomVipBadge_.set(
                    std::forward<decltype(vipBadge)>(vipBadge));
            }
        },
        [this, weak = weakOf<Channel>(this)](auto &&channelBadges) {
            if (auto shared = weak.lock())
            {
                this->tgFfzChannelBadges_.guard();
                this->ffzChannelBadges_ =
                    std::forward<decltype(channelBadges)>(channelBadges);
            }
        },
        manualRefresh, cacheHit);
}

void TwitchChannel::refreshBadgesProviders()
{
    getApp()->getHomiesBadges()->loadHomiesBadges();
    this->addSystemMessage("Homies badges reloaded.");
    getApp()->getFolhinhaBadges()->loadFolhinhaBadges();
    this->addSystemMessage("FolhinhaBot badges reloaded.");
    if (auto *badges = getApp()->getMoltorinoSupporterBadges())
    {
        badges->refreshNow();
        this->addSystemMessage("Moltorino badges reloaded.");
    }
}

void TwitchChannel::refreshSevenTVChannelEmotes(bool manualRefresh)
{
    if (!Settings::instance().enableSevenTVChannelEmotes)
    {
        this->seventvEmotes_.set(EMPTY_EMOTE_MAP);
        return;
    }

    bool cacheHit = readProviderEmotesCache(
        this->roomId(), "seventv", [this](auto jsonDoc) {
            const auto json = jsonDoc.object();
            const auto emoteSet = json["emote_set"].toObject();
            const auto parsedEmotes = emoteSet["emotes"].toArray();
            auto emoteMap = seventv::detail::parseEmotes(
                parsedEmotes, SeventvEmoteSetKind::Channel);
            this->setSeventvEmotes(std::make_shared<const EmoteMap>(emoteMap));
        });

    SeventvEmotes::loadChannelEmotes(
        weakOf<Channel>(this), this->roomId(),
        [this, weak = weakOf<Channel>(this)](auto &&emoteMap,
                                             auto channelInfo) {
            if (auto shared = weak.lock())
            {
                this->setSeventvEmotes(
                    std::make_shared<const EmoteMap>(emoteMap));
                this->updateSeventvData(channelInfo.userID,
                                        channelInfo.emoteSetID);
                this->seventvUserTwitchConnectionIndex_ =
                    channelInfo.twitchConnectionIndex;
            }
        },
        manualRefresh, cacheHit);
}

void TwitchChannel::setBttvEmotes(std::shared_ptr<const EmoteMap> &&map)
{
    this->bttvEmotes_.set(std::move(map));
}

void TwitchChannel::setFfzEmotes(std::shared_ptr<const EmoteMap> &&map)
{
    this->ffzEmotes_.set(std::move(map));
}

void TwitchChannel::setSeventvEmotes(std::shared_ptr<const EmoteMap> &&map)
{
    this->seventvEmotes_.set(std::move(map));
}

void TwitchChannel::addQueuedRedemption(const QString &rewardId,
                                        const QString &originalContent,
                                        Communi::IrcMessage *message)
{
    this->waitingRedemptions_.push_back({
        rewardId,
        originalContent,
        {message->clone(), {}},
    });
}

void TwitchChannel::addChannelPointReward(const ChannelPointReward &reward)
{
    assertInGuiThread();

    if (!reward.redemptionKey.isEmpty() &&
        !this->markChannelPointRedemptionSeen(u"pubsub:" %
                                              reward.redemptionKey))
    {
        return;
    }

    if (!reward.isUserInputRequired)
    {
        this->addMessage(MessageBuilder::makeChannelPointRewardMessage(
                             reward, this->isMod(), this->isBroadcaster()),
                         MessageContext::Original);
        return;
    }

    bool result = false;
    {
        auto channelPointRewards = this->channelPointRewards_.access();
        result = channelPointRewards->try_emplace(reward.id, reward).second;
    }
    if (result)
    {
        const auto &channelName = this->getName();
        qCDebug(chatterinoTwitch)
            << "[TwitchChannel" << channelName
            << "] Channel point reward added:" << reward.id << ","
            << reward.title << "," << reward.isUserInputRequired;

        auto *server = getApp()->getTwitch();
        auto it = std::remove_if(
            this->waitingRedemptions_.begin(), this->waitingRedemptions_.end(),
            [&](const QueuedRedemption &msg) {
                if (reward.id == msg.rewardID)
                {
                    VectorMessageSink sink(
                        MessageSinkTrait::AddMentionsToGlobalChannel);
                    IrcMessageHandler::instance().addMessage(
                        msg.message.get(), sink, this, msg.originalContent,
                        *server, false, false);
                    if (sink.messages().empty())
                    {
                        return true;
                    }
                    MessagePtr next = sink.messages().back();
                    auto prev = this->findMessageByID(next->id);
                    if (!prev)
                    {
                        // message gone
                        this->addMessage(next, MessageContext::Repost);
                        return true;
                    }
                    this->replaceMessage(prev, next);
                    return true;
                }
                return false;
            });
        this->waitingRedemptions_.erase(it, this->waitingRedemptions_.end());
    }
}

bool TwitchChannel::markChannelPointRedemptionSeen(const QString &key)
{
    assertInGuiThread();

    if (key.isEmpty())
    {
        return true;
    }

    auto &recent = this->recentChannelPointRedemptions_;
    if (std::find(recent.begin(), recent.end(), key) != recent.end())
    {
        return false;
    }

    recent.push_back(key);
    return true;
}

void TwitchChannel::addKnownChannelPointReward(const ChannelPointReward &reward)
{
    assert(getApp()->isTest());

    auto channelPointRewards = this->channelPointRewards_.access();
    channelPointRewards->try_emplace(reward.id, reward);
}

bool TwitchChannel::isChannelPointRewardKnown(const QString &rewardId)
{
    const auto &pointRewards = this->channelPointRewards_.accessConst();
    const auto &it = pointRewards->find(rewardId);
    return it != pointRewards->end();
}

std::optional<ChannelPointReward> TwitchChannel::channelPointReward(
    const QString &rewardId) const
{
    auto rewards = this->channelPointRewards_.accessConst();
    auto it = rewards->find(rewardId);

    if (it == rewards->end())
    {
        return std::nullopt;
    }
    return it->second;
}

void TwitchChannel::updateStreamStatus(
    const std::optional<HelixStream> &helixStream, bool isInitialUpdate)
{
    if (helixStream)
    {
        auto stream = *helixStream;
        {
            auto status = this->streamStatus_.access();
            status->streamId = stream.id;
            status->viewerCount = stream.viewerCount;
            status->gameId = stream.gameId;
            status->game = stream.gameName;
            status->title = stream.title;
            QDateTime since =
                QDateTime::fromString(stream.startedAt, Qt::ISODate);
            auto diff = since.secsTo(QDateTime::currentDateTime());
            status->uptime = QString::number(diff / 3600) + "h " +
                             QString::number(diff % 3600 / 60) + "m";
            status->uptimeSeconds = diff;

            status->rerun = false;
            status->streamType = stream.type;
            for (const auto &tag : stream.tags)
            {
                if (QString::compare(tag, "Rerun", Qt::CaseInsensitive) == 0)
                {
                    status->rerun = true;
                    status->streamType = "rerun";
                    break;
                }
            }
        }
        if (this->setLive(true))
        {
            this->onLiveStatusChanged(true, isInitialUpdate);
        }
        this->streamStatusChanged.invoke();
    }
    else
    {
        if (this->setLive(false))
        {
            this->onLiveStatusChanged(false, isInitialUpdate);
            this->streamStatusChanged.invoke();
        }
    }
}

void TwitchChannel::onLiveStatusChanged(bool isLive, bool isInitialUpdate)
{
    // Similar code exists in NotificationController::updateFakeChannel.
    // Since we're a TwitchChannel, we also send a message here.
    if (isLive)
    {
        qCDebug(chatterinoTwitch).nospace().noquote()
            << "[TwitchChannel " << this->getName() << "] Online";

        getApp()->getNotifications()->notifyTwitchChannelLive({
            .channelId = this->roomId(),
            .channelName = this->getName(),
            .displayName = this->getDisplayName(),
            .title = this->accessStreamStatus()->title,
            .isInitialUpdate = isInitialUpdate,
        });

        // Channel live message
        this->addMessage(
            MessageBuilder::makeLiveMessage(
                this->getDisplayName(), this->roomId(),
                this->accessStreamStatus()->title,
                {MessageFlag::System, MessageFlag::DoNotTriggerNotification}),
            MessageContext::Original);
    }
    else
    {
        qCDebug(chatterinoTwitch).nospace().noquote()
            << "[TwitchChannel " << this->getName() << "] Offline";

        // Channel offline message
        this->addMessage(MessageBuilder::makeOfflineSystemMessage(
                             this->getDisplayName(), this->roomId()),
                         MessageContext::Original);

        getApp()->getNotifications()->notifyTwitchChannelOffline(
            this->roomId());
    }
};

void TwitchChannel::updateStreamTitle(const QString &title)
{
    {
        auto status = this->streamStatus_.access();
        if (status->title == title)
        {
            // Title has not changed
            return;
        }
        status->title = title;
    }
    this->streamStatusChanged.invoke();
}

void TwitchChannel::updateDisplayName(const QString &displayName)
{
    if (displayName == this->nameOptions.actualDisplayName)
    {
        // Display name has not changed
        return;
    }

    // Display name has changed

    this->nameOptions.actualDisplayName = displayName;

    if (QString::compare(displayName, this->getName(), Qt::CaseInsensitive) ==
        0)
    {
        // Display name is only a case variation of the login name
        this->setDisplayName(displayName);

        this->setLocalizedName(displayName);
    }
    else
    {
        // Display name contains Chinese, Japanese, or Korean characters
        this->setDisplayName(this->getName());

        this->setLocalizedName(
            QString("%1(%2)").arg(this->getName()).arg(displayName));
    }

    this->addRecentChatter(this->getDisplayName());

    this->displayNameChanged.invoke();
}

void TwitchChannel::showLoginMessage()
{
    const auto linkColor = MessageColor(MessageColor::Link);
    const auto accountsLink = Link(Link::OpenAccountsPage, QString());
    const auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    const auto expirationText =
        QStringLiteral("You need to log in to send messages. You can link your "
                       "Twitch account");
    const auto loginPromptText = QStringLiteral("in the settings.");

    auto builder = MessageBuilder();
    builder.message().flags.set(MessageFlag::System);
    builder.message().flags.set(MessageFlag::DoNotTriggerNotification);

    builder.emplace<TimestampElement>();
    builder.emplace<TextElement>(expirationText, MessageElementFlag::Text,
                                 MessageColor::System);
    builder
        .emplace<TextElement>(loginPromptText, MessageElementFlag::Text,
                              linkColor)
        ->setLink(accountsLink);

    this->addMessage(builder.release(), MessageContext::Original);
}

void TwitchChannel::showAnonymousReadOnlyMessage()
{
    this->addSystemMessage("Anonymous channels are read only.");
}

void TwitchChannel::roomIdChanged()
{
    if (getApp()->isTest())
    {
        return;
    }
    const auto roomId = this->roomId();
    this->refreshPubSub();
    this->refreshBadges();
    this->refreshCheerEmotes();
    this->refreshTwitchChannelEmotes(false);
    this->joinBttvChannel();
    this->listenSevenTVCosmetics();
    getApp()->getTwitchLiveController()->add(
        std::dynamic_pointer_cast<TwitchChannel>(shared_from_this()));

    this->refreshFFZChannelEmotes(false);
    this->refreshBTTVChannelEmotes(false);
    this->refreshSevenTVChannelEmotes(false);
}

QString TwitchChannel::prepareMessage(const QString &message,
                                      int duplicateNonce) const
{
    auto *app = getApp();
    QString parsedMessage =
        app->getEmotes()->getEmojis()->replaceShortCodes(message);

    parsedMessage = parsedMessage.simplified();

    if (parsedMessage.isEmpty())
    {
        return "";
    }

    if (duplicateNonce > 0)
    {
        parsedMessage.append(duplicateBypassSuffix(duplicateNonce));
    }
    else if (!this->hasHighRateLimit())
    {
        if (getSettings()->allowDuplicateMessages)
        {
            if (parsedMessage == this->lastSentMessage_)
            {
                auto spaceIndex = parsedMessage.indexOf(' ');
                // If the message starts with either '/' or '.' Twitch will treat it as a command, omitting
                // first space and only rest of the arguments treated as actual message content
                // In cases when user sends a message like ". .a b" first character and first space are omitted as well
                bool ignoreFirstSpace =
                    parsedMessage.at(0) == '/' || parsedMessage.at(0) == '.';
                if (ignoreFirstSpace)
                {
                    spaceIndex = parsedMessage.indexOf(' ', spaceIndex + 1);
                }

                if (spaceIndex == -1)
                {
                    // no spaces found, fall back to old magic character
                    parsedMessage.append(MAGIC_MESSAGE_SUFFIX);
                }
                else
                {
                    // replace the space we found in spaceIndex with two spaces
                    parsedMessage.replace(spaceIndex, 1, "  ");
                }
            }
        }
    }

    return parsedMessage;
}

bool TwitchChannel::sendMessageViaIrc(const QString &message,
                                      int duplicateNonce)
{
    if (this->isAnonymous())
    {
        if (!message.isEmpty())
        {
            this->showAnonymousReadOnlyMessage();
        }

        return false;
    }

    auto *app = getApp();
    if (!app->getAccounts()->twitch.isLoggedIn())
    {
        if (!message.isEmpty())
        {
            this->showLoginMessage();
        }

        return false;
    }

    QString parsedMessage = this->prepareMessage(message, duplicateNonce);
    if (parsedMessage.isEmpty())
    {
        return false;
    }

    // /spam intentionally runs faster than Chatterino's normal client-side
    // send limiter. Twitch can still reject or rate-limit these server-side.
    app->getTwitch()->sendMessage(this->getName(), parsedMessage);
    this->updateBttvActivity();
    this->updateSevenTVActivity();
    this->lastSentMessage_ = parsedMessage;
    return true;
}

bool TwitchChannel::sendSpamMessageViaHelix(
    const QString &message, int duplicateNonce,
    std::function<void(bool sent, QString error)> callback)
{
    if (this->isAnonymous())
    {
        if (!message.isEmpty())
        {
            this->showAnonymousReadOnlyMessage();
        }

        return false;
    }

    auto *app = getApp();
    if (!app->getAccounts()->twitch.isLoggedIn())
    {
        if (!message.isEmpty())
        {
            this->showLoginMessage();
        }

        return false;
    }

    const auto broadcasterID = this->roomId();
    if (broadcasterID.isEmpty())
    {
        this->addSystemMessage(
            "Sending messages in this channel isn't possible.");
        return false;
    }

    QString parsedMessage = this->prepareMessage(message, duplicateNonce);
    if (parsedMessage.isEmpty())
    {
        return false;
    }

    auto sharedCallback =
        std::make_shared<std::function<void(bool sent, QString error)>>(
            std::move(callback));

    getHelix()->sendChatMessage(
        {
            .broadcasterID = broadcasterID,
            .senderID = app->getAccounts()->twitch.getCurrent()->getUserId(),
            .message = parsedMessage,
        },
        [weak = weakOf<Channel>(this), parsedMessage,
         sharedCallback](const auto &res) {
            auto channel =
                std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
            if (!channel)
            {
                return;
            }

            if (!res.isSent)
            {
                const auto error =
                    res.dropReason
                        ? res.dropReason->message
                        : QStringLiteral("Your message was not sent.");
                if (isWarningAcknowledgeNotice(error))
                {
                    channel->handleChatWarningNotice();
                }
                if (*sharedCallback)
                {
                    (*sharedCallback)(false, error);
                }
                return;
            }

            channel->updateBttvActivity();
            channel->updateSevenTVActivity();
            channel->lastSentMessage_ = parsedMessage;

            if (*sharedCallback)
            {
                (*sharedCallback)(true, {});
            }
        },
        [weak = weakOf<Channel>(this), sharedCallback](auto error,
                                                       auto message) {
            auto channel =
                std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
            if (!channel)
            {
                return;
            }

            if (*sharedCallback)
            {
                auto errorText = helixSendMessageErrorText(error, message);
                if (isWarningAcknowledgeNotice(errorText))
                {
                    channel->handleChatWarningNotice();
                }
                (*sharedCallback)(false, errorText);
            }
        });

    return true;
}

void TwitchChannel::sendBotMessage(const QString &message)
{
    if (this->isAnonymous())
    {
        if (!message.isEmpty())
        {
            this->showAnonymousReadOnlyMessage();
        }

        return;
    }

    const auto botConfig = getBotBadgeSendConfig();
    if (!botConfig.isValid())
    {
        this->addSystemMessage(
            "Bot mode is locked. Ask Molto about it. Usage: /bot <message>");
        return;
    }

    const auto broadcasterID = this->roomId();
    if (broadcasterID.isEmpty())
    {
        this->addSystemMessage(
            "Cannot send message: channel ID not available.");
        return;
    }

    QJsonObject json{{
        {"broadcaster_id", broadcasterID},
        {"sender_id", botConfig.senderID},
        {"message", message},
    }};

    NetworkRequest("https://api.twitch.tv/helix/chat/messages",
                   NetworkRequestType::Post)
        .timeout(10000)
        .header("Accept", "application/json")
        .header("Authorization", "Bearer " + botConfig.appToken)
        .header("Client-ID", botConfig.clientId)
        .header("Content-Type", "application/json")
        .json(json)
        .onSuccess([weak = weakOf<Channel>(this)](const NetworkResult &result) {
            auto channel =
                std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
            if (!channel)
            {
                return;
            }

            auto responseJson = result.parseJson();
            auto data = responseJson.value("data").toArray();
            if (data.isEmpty())
            {
                return;
            }

            auto first = data.first().toObject();
            if (first.value("is_sent").toBool())
            {
                return;
            }

            auto dropReason = first.value("drop_reason").toObject();
            if (!dropReason.isEmpty())
            {
                channel->addSystemMessage(
                    "Bot message dropped: " +
                    dropReason.value("message").toString());
            }
            else
            {
                channel->addSystemMessage("Bot message was not sent.");
            }
        })
        .onError([weak = weakOf<Channel>(this)](const NetworkResult &result) {
            auto channel =
                std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
            if (!channel)
            {
                return;
            }

            auto json = result.parseJson();
            auto msg = json.value("message").toString();
            if (msg.isEmpty())
            {
                msg = result.formatError();
            }

            if (result.status() && *result.status() == 401)
            {
                channel->addSystemMessage(
                    "Bot badge token expired or invalid.");
            }
            else
            {
                channel->addSystemMessage("Bot message failed: " + msg);
            }
        })
        .execute();
}

void TwitchChannel::sendMessage(const QString &message)
{
    if (this->isAnonymous())
    {
        if (!message.isEmpty())
        {
            this->showAnonymousReadOnlyMessage();
        }

        return;
    }

    auto *app = getApp();
    if (!app->getAccounts()->twitch.isLoggedIn())
    {
        if (!message.isEmpty())
        {
            this->showLoginMessage();
        }

        return;
    }

    qCDebug(chatterinoTwitch)
        << "[TwitchChannel" << this->getName() << "] Send message:" << message;

    // Do last message processing
    QString parsedMessage = this->prepareMessage(message);
    if (parsedMessage.isEmpty())
    {
        return;
    }

    if (getSettings()->shouldSendHelixChat() && isUnknownCommand(parsedMessage))
    {
        this->addSystemMessage(QString("%1 is not a known command.")
                                   .arg(parsedMessage.split(' ').first()));
        return;
    }

    auto sendNormally = [this, &parsedMessage] {
        bool messageSent = false;
        this->sendMessageSignal.invoke(parsedMessage, messageSent);
        this->updateBttvActivity();
        this->updateSevenTVActivity();

        if (messageSent)
        {
            qCDebug(chatterinoTwitch) << "sent";
            this->lastSentMessage_ = parsedMessage;
        }
    };

    // Intercept for Bot Badge mode
    if (shouldUseBotBadgeForSelectedAccount())
    {
        const auto botConfig = getBotBadgeSendConfig();
        const auto broadcasterID = this->roomId();
        const auto botSenderID = botConfig.senderID;
        const bool allowSelectedAccountFallback =
            selectedAccountIsBotBadgeSender(botSenderID);

        if (botConfig.isValid() && !broadcasterID.isEmpty())
        {
            QJsonObject json{{
                {"broadcaster_id", broadcasterID},
                {"sender_id", botConfig.senderID},
                {"message", parsedMessage},
            }};

            NetworkRequest("https://api.twitch.tv/helix/chat/messages",
                           NetworkRequestType::Post)
                .timeout(10000)
                .header("Authorization", "Bearer " + botConfig.appToken)
                .header("Client-Id", botConfig.clientId)
                .header("Content-Type", "application/json")
                .payload(QJsonDocument(json).toJson())
                .onSuccess([weak = weakOf<Channel>(this), parsedMessage,
                            botSenderID](const NetworkResult &result) {
                    auto channel =
                        std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
                    if (!channel)
                    {
                        return;
                    }

                    auto responseJson = result.parseJson();
                    auto data = responseJson.value("data").toArray();
                    if (!data.isEmpty())
                    {
                        auto first = data.first().toObject();
                        if (!first.value("is_sent").toBool())
                        {
                            qCWarning(chatterinoTwitch)
                                << "Bot badge send was dropped:"
                                << first.value("drop_reason").toObject();
                            if (!selectedAccountIsBotBadgeSender(botSenderID))
                            {
                                auto dropReason =
                                    first.value("drop_reason").toObject();
                                auto message =
                                    dropReason.value("message").toString();
                                channel->addSystemMessage(
                                    message.isEmpty()
                                        ? QStringLiteral(
                                              "Bot message was not sent.")
                                        : QStringLiteral(
                                              "Bot message dropped: ") +
                                              message);
                                return;
                            }

                            bool messageSent = false;
                            channel->sendMessageSignal.invoke(parsedMessage,
                                                              messageSent);
                            channel->updateBttvActivity();
                            channel->updateSevenTVActivity();
                            if (messageSent)
                            {
                                channel->lastSentMessage_ = parsedMessage;
                            }
                            return;
                        }
                    }

                    qCDebug(chatterinoTwitch)
                        << "Bot message sent via bot badge";
                    channel->updateBttvActivity();
                    channel->updateSevenTVActivity();
                    channel->lastSentMessage_ = parsedMessage;
                })
                .onError([weak = weakOf<Channel>(this), parsedMessage,
                          botSenderID](auto result) {
                    auto channel =
                        std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
                    if (!channel)
                    {
                        return;
                    }

                    qCWarning(chatterinoTwitch)
                        << "Bot badge send failed:" << result.formatError();
                    if (!selectedAccountIsBotBadgeSender(botSenderID))
                    {
                        auto json = result.parseJson();
                        auto msg = json.value("message").toString();
                        if (msg.isEmpty())
                        {
                            msg = result.formatError();
                        }

                        if (result.status() && *result.status() == 401)
                        {
                            channel->addSystemMessage(
                                "Bot badge token expired or invalid.");
                        }
                        else
                        {
                            channel->addSystemMessage("Bot message failed: " +
                                                      msg);
                        }
                        return;
                    }

                    bool messageSent = false;
                    channel->sendMessageSignal.invoke(parsedMessage,
                                                      messageSent);
                    channel->updateBttvActivity();
                    channel->updateSevenTVActivity();
                    if (messageSent)
                    {
                        channel->lastSentMessage_ = parsedMessage;
                    }
                })
                .execute();

            return;
        }

        if (!allowSelectedAccountFallback)
        {
            this->addSystemMessage(
                "Bot message failed: bot badge setup is unavailable.");
            return;
        }
    }

    sendNormally();
}

void TwitchChannel::sendReply(const QString &message, const QString &replyId)
{
    if (this->isAnonymous())
    {
        if (!message.isEmpty())
        {
            this->showAnonymousReadOnlyMessage();
        }

        return;
    }

    auto *app = getApp();
    if (!app->getAccounts()->twitch.isLoggedIn())
    {
        if (!message.isEmpty())
        {
            this->showLoginMessage();
        }

        return;
    }

    qCDebug(chatterinoTwitch) << "[TwitchChannel" << this->getName()
                              << "] Send reply message:" << message;

    // Do last message processing
    QString parsedMessage = this->prepareMessage(message);
    if (parsedMessage.isEmpty())
    {
        return;
    }

    if (getSettings()->shouldSendHelixChat() && isUnknownCommand(parsedMessage))
    {
        this->addSystemMessage(QString("%1 is not a known command.")
                                   .arg(parsedMessage.split(' ').first()));
        return;
    }

    auto sendNormally = [this, &parsedMessage, &replyId] {
        bool messageSent = false;
        this->sendReplySignal.invoke(parsedMessage, replyId, messageSent);

        if (messageSent)
        {
            qCDebug(chatterinoTwitch) << "sent";
            this->lastSentMessage_ = parsedMessage;
        }
    };

    // Intercept for Bot Badge mode
    if (shouldUseBotBadgeForSelectedAccount())
    {
        const auto botConfig = getBotBadgeSendConfig();
        const auto broadcasterID = this->roomId();
        const auto botSenderID = botConfig.senderID;
        const bool allowSelectedAccountFallback =
            selectedAccountIsBotBadgeSender(botSenderID);

        if (botConfig.isValid() && !broadcasterID.isEmpty())
        {
            QJsonObject json{{
                {"broadcaster_id", broadcasterID},
                {"sender_id", botConfig.senderID},
                {"message", parsedMessage},
                {"reply_parent_message_id", replyId},
            }};

            NetworkRequest("https://api.twitch.tv/helix/chat/messages",
                           NetworkRequestType::Post)
                .timeout(10000)
                .header("Authorization", "Bearer " + botConfig.appToken)
                .header("Client-Id", botConfig.clientId)
                .header("Content-Type", "application/json")
                .payload(QJsonDocument(json).toJson())
                .onSuccess([weak = weakOf<Channel>(this), parsedMessage,
                            replyId, botSenderID](const NetworkResult &result) {
                    auto channel =
                        std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
                    if (!channel)
                    {
                        return;
                    }

                    auto responseJson = result.parseJson();
                    auto data = responseJson.value("data").toArray();
                    if (!data.isEmpty())
                    {
                        auto first = data.first().toObject();
                        if (!first.value("is_sent").toBool())
                        {
                            qCWarning(chatterinoTwitch)
                                << "Bot badge reply was dropped:"
                                << first.value("drop_reason").toObject();
                            if (!selectedAccountIsBotBadgeSender(botSenderID))
                            {
                                auto dropReason =
                                    first.value("drop_reason").toObject();
                                auto message =
                                    dropReason.value("message").toString();
                                channel->addSystemMessage(
                                    message.isEmpty()
                                        ? QStringLiteral(
                                              "Bot reply was not sent.")
                                        : QStringLiteral(
                                              "Bot reply dropped: ") +
                                              message);
                                return;
                            }

                            bool messageSent = false;
                            channel->sendReplySignal.invoke(
                                parsedMessage, replyId, messageSent);
                            if (messageSent)
                            {
                                channel->lastSentMessage_ = parsedMessage;
                            }
                            return;
                        }
                    }

                    qCDebug(chatterinoTwitch) << "Bot reply sent via bot badge";
                    channel->lastSentMessage_ = parsedMessage;
                })
                .onError([weak = weakOf<Channel>(this), parsedMessage, replyId,
                          botSenderID](auto result) {
                    auto channel =
                        std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
                    if (!channel)
                    {
                        return;
                    }

                    qCWarning(chatterinoTwitch)
                        << "Bot badge reply failed:" << result.formatError();
                    if (!selectedAccountIsBotBadgeSender(botSenderID))
                    {
                        auto json = result.parseJson();
                        auto msg = json.value("message").toString();
                        if (msg.isEmpty())
                        {
                            msg = result.formatError();
                        }

                        if (result.status() && *result.status() == 401)
                        {
                            channel->addSystemMessage(
                                "Bot badge token expired or invalid.");
                        }
                        else
                        {
                            channel->addSystemMessage("Bot reply failed: " +
                                                      msg);
                        }
                        return;
                    }

                    bool messageSent = false;
                    channel->sendReplySignal.invoke(parsedMessage, replyId,
                                                    messageSent);
                    if (messageSent)
                    {
                        channel->lastSentMessage_ = parsedMessage;
                    }
                })
                .execute();

            return;
        }

        if (!allowSelectedAccountFallback)
        {
            this->addSystemMessage(
                "Bot reply failed: bot badge setup is unavailable.");
            return;
        }
    }

    sendNormally();
}

bool TwitchChannel::isMod() const
{
    return this->mod_;
}

bool TwitchChannel::isLeadMod() const
{
    return this->leadMod_;
}

bool TwitchChannel::isVip() const
{
    return this->vip_;
}

bool TwitchChannel::isStaff() const
{
    return this->staff_;
}

bool TwitchChannel::isFollowing() const
{
    return this->following_;
}

bool TwitchChannel::isFollowingStatusKnown() const
{
    return this->followingStatusKnown_;
}

void TwitchChannel::setFollowingStatus(bool following,
                                       std::optional<QDateTime> followedAt)
{
    const bool changed = this->followingStatusKnown_ != true ||
                         this->following_ != following ||
                         this->followedAt_ != followedAt;

    this->following_ = following;
    this->followingStatusKnown_ = true;
    this->followedAt_ = std::move(followedAt);
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (account && !account->isAnon())
    {
        this->followingStatusUserId_ = account->getUserId();
    }

    if (changed)
    {
        this->followingStatusChanged.invoke();
    }
}

void TwitchChannel::refreshFollowingStatus(bool force)
{
    if (getApp()->isTest())
    {
        return;
    }

    const auto roomId = this->roomId();
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    const auto userId =
        account && !account->isAnon() ? account->getUserId() : QString();

    auto clearUnknown = [this] {
        const bool changed = this->followingStatusKnown_ || this->following_;
        this->following_ = false;
        this->followingStatusKnown_ = false;
        this->followedAt_.reset();
        this->followingStatusUserId_.clear();
        if (changed)
        {
            this->followingStatusChanged.invoke();
        }
    };

    if (roomId.isEmpty() || userId.isEmpty())
    {
        clearUnknown();
        return;
    }

    if (userId == roomId)
    {
        this->followingStatusUserId_ = userId;
        this->setFollowingStatus(false);
        return;
    }

    if (!force && this->followingStatusKnown_ &&
        this->followingStatusUserId_ == userId)
    {
        return;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    if (!force && this->lastFollowingStatusRefreshAt_.isValid() &&
        this->lastFollowingStatusRefreshAt_.msecsTo(now) <
            FOLLOWING_STATUS_RETRY_INTERVAL_MS)
    {
        return;
    }

    if (this->followingStatusFetchInFlight_.exchange(true))
    {
        return;
    }

    this->lastFollowingStatusRefreshAt_ = now;
    const auto requestUserId = userId;
    const auto requestRoomId = roomId;
    const auto weak = this->weak_from_this();

    getHelix()->getFollowedChannel(
        requestUserId, requestRoomId, &this->lifetimeGuard_,
        [weak, requestUserId, requestRoomId](const auto &chan) {
            auto shared = std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
            if (!shared)
            {
                return;
            }

            shared->followingStatusFetchInFlight_.store(false);

            auto current = getApp()->getAccounts()->twitch.getCurrent();
            const auto currentUserId = current && !current->isAnon()
                                           ? current->getUserId()
                                           : QString();
            if (currentUserId != requestUserId ||
                shared->roomId() != requestRoomId)
            {
                shared->refreshFollowingStatus(true);
                return;
            }

            shared->followingStatusUserId_ = requestUserId;
            shared->setFollowingStatus(
                chan.has_value(),
                chan ? std::optional<QDateTime>(chan->followedAt)
                     : std::nullopt);
        },
        [weak](const auto &error) {
            auto shared = std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
            if (!shared)
            {
                return;
            }

            shared->followingStatusFetchInFlight_.store(false);
            qCDebug(chatterinoTwitch)
                << "Failed to refresh following status for" << shared->getName()
                << ':' << error;
        });
}

void TwitchChannel::setMod(bool value)
{
    if (this->mod_ != value)
    {
        this->mod_ = value;

        this->userStateChanged.invoke();
    }
}

void TwitchChannel::setLeadMod(bool value, bool known)
{
    if (this->leadMod_ != value || this->leadModStatusKnown_ != known)
    {
        const bool changedValue = this->leadMod_ != value;
        const bool wasKnown = this->leadModStatusKnown_;

        this->leadMod_ = value;
        this->leadModStatusKnown_ = known;
        if (known)
        {
            this->leadModLookupAttempted_ = true;
        }
        else if (changedValue || wasKnown)
        {
            this->leadModLookupAttempted_ = false;
        }

        if (changedValue)
        {
            this->userStateChanged.invoke();
        }
    }
}

void TwitchChannel::refreshLeadModStatus(bool force)
{
    const bool needsLeadModForUsercardActions =
        getSettings()->hideModActionsOnModUsercards &&
        getSettings()->showModActionsOnModUsercardsAsLeadMod;
    if (!getSettings()->showLeadModRoleButtons &&
        !needsLeadModForUsercardActions)
    {
        return;
    }

    if (this->leadModStatusKnown_ && !force)
    {
        return;
    }

    if (this->isBroadcaster())
    {
        this->setLeadMod(false, true);
        return;
    }

    if (this->getName().isEmpty())
    {
        return;
    }

    auto auth = MoltorinoAuth::resolveCurrentUserToken();
    if (!auth.hasToken())
    {
        this->setLeadMod(false, true);
        return;
    }
    const auto requestToken = auth.token;

    if (this->leadModLookupAttempted_ && !force)
    {
        return;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    if (!force && this->lastLeadModRefreshAt_.isValid() &&
        this->lastLeadModRefreshAt_.msecsTo(now) < LEAD_MOD_RETRY_INTERVAL_MS)
    {
        return;
    }

    if (this->leadModFetchInFlight_.exchange(true))
    {
        return;
    }
    this->leadModLookupAttempted_ = true;
    this->lastLeadModRefreshAt_ = now;

    const auto weak = this->weak_from_this();
    TwitchGql::getChannelSelfData(
        this->getName(), requestToken,
        [weak, requestToken](GqlChannelSelfData data) {
            runInGuiThread([weak, requestToken, data] {
                auto shared = weak.lock();
                if (!shared)
                {
                    return;
                }

                auto *channel = dynamic_cast<TwitchChannel *>(shared.get());
                if (!channel)
                {
                    return;
                }

                channel->leadModFetchInFlight_.store(false);

                const auto currentAuth =
                    MoltorinoAuth::resolveCurrentUserToken();
                if (!currentAuth.hasToken() ||
                    currentAuth.token != requestToken)
                {
                    channel->setLeadMod(false, false);
                    channel->leadModLookupAttempted_ = false;
                    channel->refreshLeadModStatus(true);
                    return;
                }

                channel->setLeadMod(data.isLeadModerator, true);
            });
        },
        [weak](const QString &error) {
            runInGuiThread([weak, error] {
                auto shared = weak.lock();
                if (!shared)
                {
                    return;
                }

                auto *channel = dynamic_cast<TwitchChannel *>(shared.get());
                if (!channel)
                {
                    return;
                }

                channel->leadModFetchInFlight_.store(false);
                channel->leadModLookupAttempted_ = false;
                qCDebug(chatterinoTwitch)
                    << "[LeadMod] Failed to refresh lead mod status for"
                    << channel->getName() << ':' << error;
            });
        });
}

void TwitchChannel::setVIP(bool value)
{
    if (this->vip_ != value)
    {
        this->vip_ = value;

        this->userStateChanged.invoke();
    }
}

void TwitchChannel::setStaff(bool value)
{
    if (this->staff_ != value)
    {
        this->staff_ = value;

        this->userStateChanged.invoke();
    }
}

bool TwitchChannel::isBroadcaster() const
{
    auto *app = getApp();

    return this->getName() ==
           app->getAccounts()->twitch.getCurrent()->getUserName();
}

bool TwitchChannel::hasHighRateLimit() const
{
    return this->isMod() || this->isBroadcaster() || this->isVip();
}

bool TwitchChannel::canReconnect() const
{
    return true;
}

void TwitchChannel::reconnect()
{
    if (this->isAnonymous())
    {
        getApp()->getTwitch()->reconnectAnonymousChannels();
        return;
    }

    getApp()->getTwitch()->connect();
}

QString TwitchChannel::getCurrentStreamID() const
{
    auto streamStatus = this->accessStreamStatus();
    if (streamStatus->live)
    {
        return streamStatus->streamId;
    }

    return {};
}

QString TwitchChannel::roomId() const
{
    return *this->roomID_.access();
}

void TwitchChannel::setRoomId(const QString &id)
{
    if (*this->roomID_.accessConst() != id)
    {
        *this->roomID_.access() = id;
        // This is intended for tests and benchmarks. See comment in constructor.
        if (!getApp()->isTest())
        {
            this->roomIdChanged();
            this->loadRecentMessages();
        }
        this->disconnected_ = false;
        this->lastConnectedAt_ = std::chrono::system_clock::now();
        this->roomIdAssigned.invoke();
    }
}

SharedAccessGuard<const TwitchChannel::RoomModes>
    TwitchChannel::accessRoomModes() const
{
    return this->roomModes.accessConst();
}

void TwitchChannel::setRoomModes(const RoomModes &newRoomModes)
{
    this->roomModes = newRoomModes;

    // Clear send wait timer when slow mode is disabled
    if (newRoomModes.slowMode == 0)
    {
        this->setSendWait(newRoomModes.slowMode);
    }

    this->roomModesChanged.invoke();
}

SharedAccessGuard<const std::optional<TwitchChannel::PinnedMessage>>
    TwitchChannel::accessPinnedMessage() const
{
    return this->currentPin_.accessConst();
}

void TwitchChannel::setPinnedMessage(std::optional<PinnedMessage> pin)
{
    {
        auto locked = this->currentPin_.access();
        if (!locked->has_value() && !pin.has_value())
        {
            return;
        }
        *locked = std::move(pin);
    }
    this->pinnedMessageChanged.invoke();
}

void TwitchChannel::refreshPinnedMessage()
{
    if (!getSettings()->enablePinnedMessages || this->roomId().isEmpty())
    {
        return;
    }
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    const auto weak = this->weak_from_this();
    TwitchGql::getCurrentPin(
        this->roomId(), account,
        [weak](std::optional<PinnedMessage> pin) {
            auto shared = std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
            if (!shared)
            {
                return;
            }
            if (pin)
            {
                qCDebug(chatterinoTwitch)
                    << "Found pinned message for" << shared->getName() << ":"
                    << pin->text;
            }
            else
            {
                qCDebug(chatterinoTwitch)
                    << "No pinned message for" << shared->getName();
            }
            shared->pinnedMessageRefreshFailures_ = 0;
            shared->setPinnedMessage(std::move(pin));
        },
        [weak](const QString &error) {
            auto shared = std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
            if (!shared)
            {
                return;
            }
            qCDebug(chatterinoTwitch) << "Failed to fetch pinned message for"
                                      << shared->getName() << ":" << error;
            const auto failureCount =
                shared->pinnedMessageRefreshFailures_.fetch_add(1);
            if (failureCount >= 2)
            {
                return;
            }

            const auto delayMs = failureCount == 0 ? 1500 : 5000;
            QTimer::singleShot(delayMs, [weak] {
                if (isAppAboutToQuit())
                {
                    return;
                }

                auto retry =
                    std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
                if (retry)
                {
                    retry->refreshPinnedMessage();
                }
            });
        });
}

void TwitchChannel::pinMessage(const QString &messageId, int durationSeconds)
{
    QString authError;
    auto auth = MoltorinoAuth::resolveModerationToken(
        this->roomId(), this->getName(), &authError);
    if (!auth.hasToken())
    {
        this->addSystemMessage(authError);
        return;
    }

    const auto weak = this->weak_from_this();
    TwitchGql::pinMessage(
        this->roomId(), messageId, durationSeconds, auth.token, []() {},
        [weak](const QString &error) {
            auto shared = std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
            if (!shared)
            {
                return;
            }
            shared->addSystemMessage(
                "Failed to pin message: " +
                normalizeMoltorinoAuthError("pinning messages", error));
        });
}

void TwitchChannel::unpinMessage()
{
    QString authError;
    auto auth = MoltorinoAuth::resolveModerationToken(
        this->roomId(), this->getName(), &authError);
    if (!auth.hasToken())
    {
        this->addSystemMessage(authError);
        return;
    }

    auto pinGuard = this->accessPinnedMessage();
    if (!pinGuard->has_value())
    {
        this->addSystemMessage("No message is currently pinned to unpin.");
        return;
    }
    QString pinId = (*pinGuard)->pinId;

    const auto weak = this->weak_from_this();
    TwitchGql::unpinMessage(
        pinId, auth.token, []() {},
        [weak](const QString &error) {
            auto shared = std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
            if (!shared)
            {
                return;
            }
            shared->addSystemMessage(
                "Failed to unpin message: " +
                normalizeMoltorinoAuthError("unpinning messages", error));
        });
}

void TwitchChannel::keepPinned()
{
    QString authError;
    auto auth = MoltorinoAuth::resolveModerationToken(
        this->roomId(), this->getName(), &authError);
    if (!auth.hasToken())
    {
        this->addSystemMessage(authError);
        return;
    }

    auto pinGuard = this->accessPinnedMessage();
    if (!pinGuard->has_value())
    {
        return;
    }
    QString pinId = (*pinGuard)->pinId;

    const auto weak = this->weak_from_this();
    TwitchGql::updatePinnedMessage(
        pinId, std::nullopt, auth.token, []() {},
        [weak](const QString &error) {
            auto shared = std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
            if (!shared)
            {
                return;
            }
            shared->addSystemMessage(
                "Failed to keep message pinned: " +
                normalizeMoltorinoAuthError("changing pin duration", error));
        });
}

bool TwitchChannel::isLive() const
{
    return this->streamStatus_.accessConst()->live;
}

bool TwitchChannel::isRerun() const
{
    return this->streamStatus_.accessConst()->rerun;
}

SharedAccessGuard<const TwitchChannel::StreamStatus>
    TwitchChannel::accessStreamStatus() const
{
    return this->streamStatus_.accessConst();
}

std::optional<EmotePtr> TwitchChannel::twitchEmote(const EmoteName &name) const
{
    auto emotes = this->localTwitchEmotes();
    auto it = emotes->find(name);

    if (it == emotes->end())
    {
        return getApp()->getAccounts()->twitch.getCurrent()->twitchEmote(name);
    }
    return it->second;
}

std::optional<EmotePtr> TwitchChannel::bttvEmote(const EmoteName &name) const
{
    auto emotes = this->bttvEmotes_.get();
    auto it = emotes->find(name);

    if (it == emotes->end())
    {
        return std::nullopt;
    }
    return it->second;
}

std::optional<EmotePtr> TwitchChannel::ffzEmote(const EmoteName &name) const
{
    auto emotes = this->ffzEmotes_.get();
    auto it = emotes->find(name);

    if (it == emotes->end())
    {
        return std::nullopt;
    }
    return it->second;
}

std::optional<EmotePtr> TwitchChannel::seventvEmote(const EmoteName &name) const
{
    auto emotes = this->seventvEmotes_.get();
    auto it = emotes->find(name);

    if (it == emotes->end())
    {
        return std::nullopt;
    }
    return it->second;
}

std::shared_ptr<const EmoteMap> TwitchChannel::localTwitchEmotes() const
{
    return this->localTwitchEmotes_.get();
}

std::shared_ptr<const EmoteMap> TwitchChannel::bttvEmotes() const
{
    return this->bttvEmotes_.get();
}

std::shared_ptr<const EmoteMap> TwitchChannel::ffzEmotes() const
{
    return this->ffzEmotes_.get();
}

std::shared_ptr<const EmoteMap> TwitchChannel::seventvEmotes() const
{
    return this->seventvEmotes_.get();
}

const QString &TwitchChannel::seventvUserID() const
{
    return this->seventvUserID_;
}
const QString &TwitchChannel::seventvEmoteSetID() const
{
    return this->seventvEmoteSetID_;
}

void TwitchChannel::joinBttvChannel() const
{
    if (getApp()->getBttvLiveUpdates())
    {
        const auto currentAccount =
            getApp()->getAccounts()->twitch.getCurrent();
        QString userID;
        if (currentAccount && !currentAccount->isAnon())
        {
            userID = currentAccount->getUserId();
        }
        getApp()->getBttvLiveUpdates()->joinChannel(this->roomId(), userID);
    }
}

void TwitchChannel::addBttvEmote(
    const BttvLiveUpdateEmoteUpdateAddMessage &message)
{
    auto emote = BttvEmotes::addEmote(this->getDisplayName(), this->bttvEmotes_,
                                      message);

    this->addOrReplaceLiveUpdatesAddRemove(true, "BTTV", QString() /*actor*/,
                                           emote->name.string);
}

void TwitchChannel::updateBttvEmote(
    const BttvLiveUpdateEmoteUpdateAddMessage &message)
{
    auto updated = BttvEmotes::updateEmote(this->getDisplayName(),
                                           this->bttvEmotes_, message);
    if (!updated)
    {
        return;
    }

    const auto [oldEmote, newEmote] = *updated;
    if (oldEmote->name == newEmote->name)
    {
        return;  // only the creator changed
    }

    auto builder = MessageBuilder(liveUpdatesUpdateEmoteMessage, "BTTV",
                                  QString() /* actor */, newEmote->name.string,
                                  oldEmote->name.string);
    this->addMessage(builder.release(), MessageContext::Original);
}

void TwitchChannel::removeBttvEmote(
    const BttvLiveUpdateEmoteRemoveMessage &message)
{
    auto removed = BttvEmotes::removeEmote(this->bttvEmotes_, message);
    if (!removed)
    {
        return;
    }

    this->addOrReplaceLiveUpdatesAddRemove(false, "BTTV", QString() /*actor*/,
                                           (*removed)->name.string);
}

void TwitchChannel::addSeventvEmote(
    const seventv::eventapi::EmoteAddDispatch &dispatch)
{
    if (!SeventvEmotes::addEmote(this->seventvEmotes_, dispatch))
    {
        return;
    }

    this->addOrReplaceLiveUpdatesAddRemove(
        true, "7TV", dispatch.actorName, dispatch.emoteJson["name"].toString());
}

void TwitchChannel::updateSeventvEmote(
    const seventv::eventapi::EmoteUpdateDispatch &dispatch)
{
    if (!SeventvEmotes::updateEmote(this->seventvEmotes_, dispatch))
    {
        return;
    }

    auto builder =
        MessageBuilder(liveUpdatesUpdateEmoteMessage, "7TV", dispatch.actorName,
                       dispatch.emoteName, dispatch.oldEmoteName);
    this->addMessage(builder.release(), MessageContext::Original);
}

void TwitchChannel::removeSeventvEmote(
    const seventv::eventapi::EmoteRemoveDispatch &dispatch)
{
    auto removed = SeventvEmotes::removeEmote(this->seventvEmotes_, dispatch);
    if (!removed)
    {
        return;
    }

    this->addOrReplaceLiveUpdatesAddRemove(false, "7TV", dispatch.actorName,
                                           (*removed)->name.string);
}

void TwitchChannel::updateSeventvUser(
    const seventv::eventapi::UserConnectionUpdateDispatch &dispatch)
{
    if (dispatch.connectionIndex != this->seventvUserTwitchConnectionIndex_)
    {
        // A different connection was updated
        return;
    }

    this->updateSeventvData(this->seventvUserID_, dispatch.emoteSetID);
    SeventvEmotes::getEmoteSet(
        dispatch.emoteSetID,
        [this, weak = weakOf<Channel>(this), dispatch](auto &&emotes,
                                                       const auto &name) {
            postToThread([this, weak, dispatch, emotes, name]() {
                if (auto shared = weak.lock())
                {
                    this->seventvEmotes_.set(
                        std::make_shared<EmoteMap>(emotes));
                    auto builder =
                        MessageBuilder(liveUpdatesUpdateEmoteSetMessage, "7TV",
                                       dispatch.actorName, name);
                    this->addMessage(builder.release(),
                                     MessageContext::Original);
                }
            });
        },
        [this, weak = weakOf<Channel>(this)](const auto &reason) {
            postToThread([this, weak, reason]() {
                if (auto shared = weak.lock())
                {
                    this->seventvEmotes_.set(EMPTY_EMOTE_MAP);
                    this->addSystemMessage(
                        QString("Failed updating 7TV emote set (%1).")
                            .arg(reason));
                }
            });
        });
}

void TwitchChannel::updateSeventvData(const QString &newUserID,
                                      const QString &newEmoteSetID)
{
    if (this->seventvUserID_ == newUserID &&
        this->seventvEmoteSetID_ == newEmoteSetID)
    {
        return;
    }

    const auto oldUserID = makeConditionedOptional(
        !this->seventvUserID_.isEmpty() && this->seventvUserID_ != newUserID,
        this->seventvUserID_);
    const auto oldEmoteSetID =
        makeConditionedOptional(!this->seventvEmoteSetID_.isEmpty() &&
                                    this->seventvEmoteSetID_ != newEmoteSetID,
                                this->seventvEmoteSetID_);

    this->seventvUserID_ = newUserID;
    this->seventvEmoteSetID_ = newEmoteSetID;
    runInGuiThread([this, oldUserID, oldEmoteSetID]() {
        if (getApp()->getSeventvEventAPI())
        {
            getApp()->getSeventvEventAPI()->subscribeUser(
                this->seventvUserID_, this->seventvEmoteSetID_);

            if (oldUserID || oldEmoteSetID)
            {
                getApp()->getTwitch()->dropSeventvChannel(
                    oldUserID.value_or(QString()),
                    oldEmoteSetID.value_or(QString()));
            }
        }
    });
}

void TwitchChannel::addOrReplaceLiveUpdatesAddRemove(bool isEmoteAdd,
                                                     const QString &platform,
                                                     const QString &actor,
                                                     const QString &emoteName)
{
    if (this->tryReplaceLastLiveUpdateAddOrRemove(
            isEmoteAdd ? MessageFlag::LiveUpdatesAdd
                       : MessageFlag::LiveUpdatesRemove,
            platform, actor, emoteName))
    {
        return;
    }

    this->lastLiveUpdateEmoteNames_ = {emoteName};

    MessagePtr msg;
    if (isEmoteAdd)
    {
        msg = MessageBuilder(liveUpdatesAddEmoteMessage, platform, actor,
                             this->lastLiveUpdateEmoteNames_)
                  .release();
    }
    else
    {
        msg = MessageBuilder(liveUpdatesRemoveEmoteMessage, platform, actor,
                             this->lastLiveUpdateEmoteNames_)
                  .release();
    }
    this->lastLiveUpdateEmotePlatform_ = platform;
    this->lastLiveUpdateMessage_ = msg;
    this->lastLiveUpdateEmoteActor_ = actor;
    this->addMessage(msg, MessageContext::Original);
}

bool TwitchChannel::tryReplaceLastLiveUpdateAddOrRemove(
    MessageFlag op, const QString &platform, const QString &actor,
    const QString &emoteName)
{
    if (this->lastLiveUpdateEmotePlatform_ != platform)
    {
        return false;
    }
    auto last = this->lastLiveUpdateMessage_.lock();
    if (!last || !last->flags.has(op) ||
        last->parseTime < QTime::currentTime().addSecs(-5) ||
        last->loginName != actor)
    {
        return false;
    }
    // Update the message
    this->lastLiveUpdateEmoteNames_.push_back(emoteName);

    auto makeReplacement = [&](MessageFlag op) -> MessageBuilder {
        if (op == MessageFlag::LiveUpdatesAdd)
        {
            return {
                liveUpdatesAddEmoteMessage,
                platform,
                last->loginName,
                this->lastLiveUpdateEmoteNames_,
            };
        }

        // op == RemoveEmoteMessage
        return {
            liveUpdatesRemoveEmoteMessage,
            platform,
            last->loginName,
            this->lastLiveUpdateEmoteNames_,
        };
    };

    auto replacement = makeReplacement(op);

    replacement->flags = last->flags;

    auto msg = replacement.release();
    this->lastLiveUpdateMessage_ = msg;
    this->replaceMessage(last, msg);

    return true;
}

void TwitchChannel::messageRemovedFromStart(const MessagePtr &msg)
{
    if (msg->id == this->shownChatWarningMessageId_)
    {
        this->shownChatWarningMessageId_.clear();
    }

    if (msg->replyThread)
    {
        if (msg->replyThread->liveCount(msg) == 0)
        {
            this->threads_.erase(msg->replyThread->rootId());
        }
    }
}

const QString &TwitchChannel::subscriptionUrl()
{
    return this->subscriptionUrl_;
}

const QString &TwitchChannel::channelUrl()
{
    return this->channelUrl_;
}

const QString &TwitchChannel::popoutPlayerUrl()
{
    return this->popoutPlayerUrl_;
}

int TwitchChannel::chatterCount() const
{
    return this->chatterCount_;
}

bool TwitchChannel::setLive(bool newLiveStatus)
{
    auto guard = this->streamStatus_.access();
    if (guard->live == newLiveStatus)
    {
        return false;
    }
    guard->live = newLiveStatus;
    if (!newLiveStatus)
    {
        // A rerun is just a fancy livestream
        guard->rerun = false;
    }

    return true;
}

void TwitchChannel::markConnected()
{
    if (this->lastConnectedAt_.has_value() && !this->disconnected_)
    {
        this->lastConnectedAt_ = std::chrono::system_clock::now();
    }
}

void TwitchChannel::markDisconnected()
{
    if (this->roomId().isEmpty())
    {
        // we were never joined in the first place
        return;
    }

    this->leadModLookupAttempted_ = false;
    this->lastLeadModRefreshAt_ = QDateTime();
    this->setLeadMod(false, false);
    this->disconnected_ = true;
}

void TwitchChannel::loadRecentMessages()
{
    if (!getSettings()->loadTwitchMessageHistoryOnConnect)
    {
        return;
    }

    if (this->loadingRecentMessages_.test_and_set())
    {
        return;  // already loading
    }

    auto weak = weakOf<Channel>(this);
    recentmessages::load(
        this->getName(), weak,
        [weak](const auto &messages) {
            assert(!isAppAboutToQuit());
            auto shared = weak.lock();
            if (!shared)
            {
                return;
            }

            auto *tc = dynamic_cast<TwitchChannel *>(shared.get());
            if (!tc)
            {
                return;
            }

            tc->addMessagesAtStart(messages);
            tc->loadingRecentMessages_.clear();

            std::vector<MessagePtr> msgs;
            for (const auto &msg : messages)
            {
                const auto highlighted =
                    msg->flags.has(MessageFlag::Highlighted);
                const auto showInMentions =
                    msg->flags.has(MessageFlag::ShowInMentions);
                if (highlighted && showInMentions)
                {
                    msgs.push_back(msg);
                }
            }

            getApp()->getTwitch()->getMentionsChannel()->fillInMissingMessages(
                msgs);
        },
        [weak]() {
            auto shared = weak.lock();
            if (!shared)
            {
                return;
            }

            auto *tc = dynamic_cast<TwitchChannel *>(shared.get());
            if (!tc)
            {
                return;
            }

            tc->loadingRecentMessages_.clear();
        },
        getSettings()->twitchMessageHistoryLimit.getValue(), std::nullopt,
        std::nullopt, false);
}

void TwitchChannel::loadRecentMessagesReconnect()
{
    if (!getSettings()->loadTwitchMessageHistoryOnConnect)
    {
        return;
    }

    if (this->loadingRecentMessages_.test_and_set())
    {
        return;  // already loading
    }

    const auto now = std::chrono::system_clock::now();
    int limit = getSettings()->twitchMessageHistoryLimit.getValue();
    if (this->lastConnectedAt_.has_value())
    {
        // calculate how many messages could have occurred
        // while we were not connected to the channel
        // assuming a maximum of 10 messages per second
        const auto secondsSinceDisconnect =
            std::chrono::duration_cast<std::chrono::seconds>(
                now - this->lastConnectedAt_.value())
                .count();
        limit =
            std::min(static_cast<int>(secondsSinceDisconnect + 1) * 10, limit);
    }

    auto weak = weakOf<Channel>(this);
    recentmessages::load(
        this->getName(), weak,
        [weak](const auto &messages) {
            auto shared = weak.lock();
            if (!shared)
            {
                return;
            }

            auto *tc = dynamic_cast<TwitchChannel *>(shared.get());
            if (!tc)
            {
                return;
            }

            tc->fillInMissingMessages(messages);
            tc->loadingRecentMessages_.clear();
        },
        [weak]() {
            auto shared = weak.lock();
            if (!shared)
            {
                return;
            }

            auto *tc = dynamic_cast<TwitchChannel *>(shared.get());
            if (!tc)
            {
                return;
            }

            tc->loadingRecentMessages_.clear();
        },
        limit, this->lastConnectedAt_, now, true);
}

void TwitchChannel::refreshPubSub()
{
    if (getApp()->isTest())
    {
        return;
    }

    auto resetEventSubHandles = [this] {
        this->eventSubChannelModerateHandle.reset();
        this->eventSubAutomodMessageHoldHandle.reset();
        this->eventSubAutomodMessageUpdateHandle.reset();
        this->eventSubSuspiciousUserMessageHandle.reset();
        this->eventSubSuspiciousUserUpdateHandle.reset();
        this->eventSubChannelChatUserMessageHoldHandle.reset();
        this->eventSubChannelChatUserMessageUpdateHandle.reset();
    };

    if (this->isAnonymous())
    {
        resetEventSubHandles();
        return;
    }

    auto roomId = this->roomId();
    if (roomId.isEmpty())
    {
        return;
    }

    auto currentAccount = getApp()->getAccounts()->twitch.getCurrent();

    getApp()->getTwitchPubSub()->listenToChannelPointRewards(roomId);

    if (getSettings()->enablePinnedMessages)
    {
        getApp()->getTwitchPubSub()->listenToPinnedChatUpdates(roomId);
        this->refreshPinnedMessage();
    }

    if (getSettings()->enablePredictions)
    {
        getApp()->getTwitchPubSub()->listenToPredictions(roomId);
    }
    if (getSettings()->enablePolls)
    {
        getApp()->getTwitchPubSub()->listenToPolls(roomId);
    }
    if (getSettings()->showRaidStatusAboveInput)
    {
        getApp()->getTwitchPubSub()->listenToRaids(roomId);
    }

    const auto currentUserId = currentAccount->getUserId();
    if (!currentAccount->isAnon() && !currentUserId.isEmpty())
    {
        auto auth = MoltorinoAuth::resolveCurrentUserToken();
        if (auth.hasToken())
        {
            const auto authUserId =
                auth.userId.isEmpty() ? currentUserId : auth.userId;
            getApp()->getTwitchPubSub()->forgetOtherUserAuthenticatedTopics(
                authUserId);
            getApp()->getTwitchPubSub()->listenToChatWarnings(authUserId,
                                                              auth.token);

            if (getSettings()->enablePredictions)
            {
                getApp()->getTwitchPubSub()->listenToUserPredictions(
                    authUserId, auth.token);
            }

            if (getSettings()->enableChannelPointsDisplay)
            {
                getApp()->getTwitchPubSub()->listenToUserChannelPoints(
                    authUserId, auth.token);
            }
        }
        else
        {
            getApp()->getTwitchPubSub()->forgetOtherUserAuthenticatedTopics(
                QString());
        }
    }

    if (currentAccount->isAnon())
    {
        getApp()->getTwitchPubSub()->forgetOtherUserAuthenticatedTopics(
            QString());
        resetEventSubHandles();
        return;
    }

    const auto &currentTwitchUserID = currentAccount->getUserId();

    if (this->hasModRights())
    {
        this->eventSubChannelModerateHandle =
            getApp()->getEventSub()->subscribe(eventsub::SubscriptionRequest{
                .subscriptionType = "channel.moderate",
                .subscriptionVersion = "2",
                .ownerTwitchUserID = currentTwitchUserID,
                .conditions =
                    {
                        {
                            "broadcaster_user_id",
                            roomId,
                        },
                        {
                            "moderator_user_id",
                            currentTwitchUserID,
                        },
                    },
            });
        this->eventSubAutomodMessageHoldHandle =
            getApp()->getEventSub()->subscribe(eventsub::SubscriptionRequest{
                .subscriptionType = "automod.message.hold",
                .subscriptionVersion = "2",
                .ownerTwitchUserID = currentTwitchUserID,
                .conditions =
                    {
                        {
                            "broadcaster_user_id",
                            roomId,
                        },
                        {
                            "moderator_user_id",
                            currentTwitchUserID,
                        },
                    },
            });
        this->eventSubAutomodMessageUpdateHandle =
            getApp()->getEventSub()->subscribe(eventsub::SubscriptionRequest{
                .subscriptionType = "automod.message.update",
                .subscriptionVersion = "2",
                .ownerTwitchUserID = currentTwitchUserID,
                .conditions =
                    {
                        {
                            "broadcaster_user_id",
                            roomId,
                        },
                        {
                            "moderator_user_id",
                            currentTwitchUserID,
                        },
                    },
            });
        this->eventSubSuspiciousUserMessageHandle =
            getApp()->getEventSub()->subscribe(eventsub::SubscriptionRequest{
                .subscriptionType = "channel.suspicious_user.message",
                .subscriptionVersion = "1",
                .ownerTwitchUserID = currentTwitchUserID,
                .conditions =
                    {
                        {
                            "broadcaster_user_id",
                            roomId,
                        },
                        {
                            "moderator_user_id",
                            currentTwitchUserID,
                        },
                    },
            });
        this->eventSubSuspiciousUserUpdateHandle =
            getApp()->getEventSub()->subscribe(eventsub::SubscriptionRequest{
                .subscriptionType = "channel.suspicious_user.update",
                .subscriptionVersion = "1",
                .ownerTwitchUserID = currentTwitchUserID,
                .conditions =
                    {
                        {
                            "broadcaster_user_id",
                            roomId,
                        },
                        {
                            "moderator_user_id",
                            currentTwitchUserID,
                        },
                    },
            });

        this->eventSubChannelChatUserMessageHoldHandle.reset();
        this->eventSubChannelChatUserMessageUpdateHandle.reset();
    }
    else
    {
        this->eventSubChannelModerateHandle.reset();
        this->eventSubAutomodMessageHoldHandle.reset();
        this->eventSubAutomodMessageUpdateHandle.reset();
        this->eventSubSuspiciousUserMessageHandle.reset();
        this->eventSubSuspiciousUserUpdateHandle.reset();

        this->eventSubChannelChatUserMessageHoldHandle =
            getApp()->getEventSub()->subscribe(eventsub::SubscriptionRequest{
                .subscriptionType = "channel.chat.user_message_hold",
                .subscriptionVersion = "1",
                .ownerTwitchUserID = currentTwitchUserID,
                .conditions =
                    {
                        {
                            "broadcaster_user_id",
                            roomId,
                        },
                        {
                            "user_id",
                            currentTwitchUserID,
                        },
                    },
            });

        this->eventSubChannelChatUserMessageUpdateHandle =
            getApp()->getEventSub()->subscribe(eventsub::SubscriptionRequest{
                .subscriptionType = "channel.chat.user_message_update",
                .subscriptionVersion = "1",
                .ownerTwitchUserID = currentTwitchUserID,
                .conditions =
                    {
                        {
                            "broadcaster_user_id",
                            roomId,
                        },
                        {
                            "user_id",
                            currentTwitchUserID,
                        },
                    },
            });
    }
}

void TwitchChannel::refreshChatters()
{
    // helix endpoint only works for mods
    if (!this->hasModRights())
    {
        return;
    }

    if (this->chatterFetchInFlight_.load())
    {
        return;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    if (this->lastChatterRefreshAt_.isValid() &&
        this->lastChatterRefreshAt_.msecsTo(now) < 60000)
    {
        return;
    }

    this->chatterFetchInFlight_.store(true);

    // setting?
    const auto streamStatus = this->accessStreamStatus();
    const auto viewerCount = static_cast<int>(streamStatus->viewerCount);
    if (getSettings()->onlyFetchChattersForSmallerStreamers)
    {
        if (streamStatus->live &&
            viewerCount > getSettings()->smallStreamerLimit)
        {
            this->chatterFetchInFlight_.store(false);
            return;
        }
    }

    // Get chatter list via helix api
    getHelix()->getChatters(
        this->roomId(),
        getApp()->getAccounts()->twitch.getCurrent()->getUserId(),
        MAX_CHATTERS_TO_FETCH,
        [this, weak = weakOf<Channel>(this)](auto result) {
            if (auto shared = weak.lock())
            {
                this->updateOnlineChatters(result.chatters);
                this->chatterCount_ = result.total;
                this->lastChatterRefreshAt_ = QDateTime::currentDateTimeUtc();
                this->chatterFetchInFlight_.store(false);
            }
        },
        [this, weak = weakOf<Channel>(this)](auto error, auto message) {
            if (auto shared = weak.lock())
            {
                this->chatterFetchInFlight_.store(false);
            }
            (void)error;
            (void)message;
        });
}

void TwitchChannel::addReplyThread(const std::shared_ptr<MessageThread> &thread)
{
    this->threads_[thread->rootId()] = thread;
}

const std::unordered_map<QString, std::weak_ptr<MessageThread>> &
    TwitchChannel::threads() const
{
    return this->threads_;
}

std::shared_ptr<MessageThread> TwitchChannel::getOrCreateThread(
    const MessagePtr &message)
{
    assert(message != nullptr);

    auto threadIt = this->threads_.find(message->id);
    if (threadIt != this->threads_.end() && !threadIt->second.expired())
    {
        return threadIt->second.lock();
    }

    auto thread = std::make_shared<MessageThread>(message);
    this->addReplyThread(thread);
    return thread;
}

void TwitchChannel::cleanUpReplyThreads()
{
    for (auto it = this->threads_.begin(), last = this->threads_.end();
         it != last;)
    {
        bool doErase = true;
        if (auto thread = it->second.lock())
        {
            doErase = thread->liveCount() == 0;
        }

        if (doErase)
        {
            it = this->threads_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void TwitchChannel::refreshBadges()
{
    if (this->roomId().isEmpty())
    {
        return;
    }

    getHelix()->getChannelBadges(
        this->roomId(),
        // successCallback
        [this, weak = weakOf<Channel>(this)](const auto &channelBadges) {
            auto shared = weak.lock();
            if (!shared)
            {
                // The channel has been closed inbetween us making the request and the request finishing
                return;
            }

            this->addTwitchBadgeSets(channelBadges);
        },
        // failureCallback
        [this, weak = weakOf<Channel>(this)](auto error, auto message) {
            auto shared = weak.lock();
            if (!shared)
            {
                // The channel has been closed inbetween us making the request and the request finishing
                return;
            }

            QString errorMessage("Failed to load channel badges - ");

            switch (error)
            {
                case HelixGetChannelBadgesError::Forwarded: {
                    errorMessage += message;
                }
                break;

                // This would most likely happen if the service is down, or if the JSON payload returned has changed format
                case HelixGetChannelBadgesError::Unknown: {
                    errorMessage += "An unknown error has occurred.";
                }
                break;
            }

            this->addSystemMessage(errorMessage);
        });
}

void TwitchChannel::addTwitchBadgeSets(const HelixChannelBadges &channelBadges)
{
    auto badgeSets = this->badgeSets_.access();

    for (const auto &badgeSet : channelBadges.badgeSets)
    {
        const auto &setID = badgeSet.setID;
        for (const auto &version : badgeSet.versions)
        {
            auto emote = Emote{
                .name = EmoteName{},
                .images =
                    ImageSet{
                        Image::fromUrl(version.imageURL1x, 1, BASE_BADGE_SIZE),
                        Image::fromUrl(version.imageURL2x, .5,
                                       BASE_BADGE_SIZE * 2),
                        Image::fromUrl(version.imageURL4x, .25,
                                       BASE_BADGE_SIZE * 4),
                    },
                .tooltip = Tooltip{version.title},
                .homePage = version.clickURL,
            };
            (*badgeSets)[setID][version.id] = std::make_shared<Emote>(emote);
        }
    }
}

void TwitchChannel::refreshCheerEmotes()
{
    getHelix()->getCheermotes(
        this->roomId(),
        [this, weak = weakOf<Channel>(this)](
            const std::vector<HelixCheermoteSet> &cheermoteSets) {
            auto shared = weak.lock();
            if (!shared)
            {
                return;
            }

            this->setCheerEmoteSets(cheermoteSets);
        },
        [] {
            // Failure
        });
}

void TwitchChannel::setCheerEmoteSets(
    const std::vector<HelixCheermoteSet> &cheermoteSets)
{
    std::vector<CheerEmoteSet> emoteSets;

    for (const auto &set : cheermoteSets)
    {
        auto cheerEmoteSet = CheerEmoteSet();
        cheerEmoteSet.regex =
            QRegularExpression("^" + set.prefix + "([1-9][0-9]*)$",
                               QRegularExpression::CaseInsensitiveOption);

        for (const auto &tier : set.tiers)
        {
            CheerEmote cheerEmote;

            cheerEmote.color = QColor(tier.color);
            cheerEmote.minBits = tier.minBits;
            cheerEmote.regex = cheerEmoteSet.regex;

            // TODO(pajlada): We currently hardcode dark here :|
            // We will continue to do so for now since we haven't had to
            // solve that anywhere else

            // Combine the prefix (e.g. BibleThump) with the tier (1, 100 etc.)
            auto emoteTooltip = set.prefix + tier.id + "<br>Twitch Cheer Emote";
            auto makeImageSet = [](const HelixCheermoteImage &image) {
                return ImageSet{
                    Image::fromUrl(image.imageURL1x, 1.0, BASE_BADGE_SIZE),
                    Image::fromUrl(image.imageURL2x, 0.5, BASE_BADGE_SIZE * 2),
                    Image::fromUrl(image.imageURL4x, 0.25, BASE_BADGE_SIZE * 4),
                };
            };
            cheerEmote.animatedEmote = std::make_shared<Emote>(Emote{
                .name = EmoteName{u"cheer emote"_s},
                .images = makeImageSet(tier.darkAnimated),
                .tooltip = Tooltip{emoteTooltip},
                .homePage = Url{},
            });
            cheerEmote.staticEmote = std::make_shared<Emote>(Emote{
                .name = EmoteName{u"cheer emote"_s},
                .images = makeImageSet(tier.darkStatic),
                .tooltip = Tooltip{emoteTooltip},
                .homePage = Url{},
            });

            cheerEmoteSet.cheerEmotes.emplace_back(std::move(cheerEmote));
        }

        // Sort cheermotes by cost
        std::sort(cheerEmoteSet.cheerEmotes.begin(),
                  cheerEmoteSet.cheerEmotes.end(),
                  [](const auto &lhs, const auto &rhs) {
                      return lhs.minBits > rhs.minBits;
                  });

        emoteSets.emplace_back(std::move(cheerEmoteSet));
    }

    *this->cheerEmoteSets_.access() = std::move(emoteSets);
}

void TwitchChannel::createClip(const QString &title,
                               const std::optional<int> duration)
{
    if (!this->isLive())
    {
        this->addSystemMessage(
            "Cannot create clip while the channel is offline!");
        return;
    }

    // timer has never started, proceed and start it
    if (!this->clipCreationTimer_.isValid())
    {
        this->clipCreationTimer_.start();
    }
    else if (this->clipCreationTimer_.elapsed() < CLIP_CREATION_COOLDOWN ||
             this->isClipCreationInProgress)
    {
        return;
    }

    this->addSystemMessage("Creating clip...");
    this->isClipCreationInProgress = true;

    getHelix()->createClip(
        this->roomId(), title, duration,
        // successCallback
        [this](const HelixClip &clip) {
            MessageBuilder builder;
            QString text(
                "Clip created! Copy link to clipboard or edit it in browser.");
            builder.message().messageText = text;
            builder.message().searchText = text;
            builder.message().flags.set(MessageFlag::System);

            builder.emplace<TimestampElement>();
            // text
            builder.emplace<TextElement>("Clip created!",
                                         MessageElementFlag::Text,
                                         MessageColor::System);
            // clip link
            builder
                .emplace<TextElement>("Copy link to clipboard",
                                      MessageElementFlag::Text,
                                      MessageColor::Link)
                ->setLink(Link(Link::CopyToClipboard, CLIPS_LINK.arg(clip.id)));
            // separator text
            builder.emplace<TextElement>("or", MessageElementFlag::Text,
                                         MessageColor::System);
            // edit link
            builder
                .emplace<TextElement>("edit it in browser.",
                                      MessageElementFlag::Text,
                                      MessageColor::Link)
                ->setLink(Link(Link::Url, clip.editUrl));

            this->addMessage(builder.release(), MessageContext::Original);
        },
        // failureCallback
        [this](auto error, auto errorMessage) {
            MessageBuilder builder;
            QString text;
            builder.message().flags.set(MessageFlag::System);

            builder.emplace<TimestampElement>();

            switch (error)
            {
                case HelixClipError::ClipsUnavailable: {
                    builder.emplace<TextElement>(
                        CLIPS_FAILURE_CLIPS_UNAVAILABLE_TEXT.arg(errorMessage),
                        MessageElementFlag::Text, MessageColor::System);
                    text =
                        CLIPS_FAILURE_CLIPS_UNAVAILABLE_TEXT.arg(errorMessage);
                }
                break;

                case HelixClipError::ClipsDisabled: {
                    builder.emplace<TextElement>(
                        CLIPS_FAILURE_CLIPS_DISABLED_TEXT,
                        MessageElementFlag::Text, MessageColor::System);
                    text = CLIPS_FAILURE_CLIPS_DISABLED_TEXT;
                }
                break;

                case HelixClipError::ClipsRestricted: {
                    builder.emplace<TextElement>(
                        CLIPS_FAILURE_CLIPS_RESTRICTED_TEXT,
                        MessageElementFlag::Text, MessageColor::System);
                    text = CLIPS_FAILURE_CLIPS_RESTRICTED_TEXT;
                }
                break;

                case HelixClipError::ClipsRestrictedCategory: {
                    builder.emplace<TextElement>(
                        CLIPS_FAILURE_CLIPS_RESTRICTED_CATEGORY_TEXT,
                        MessageElementFlag::Text, MessageColor::System);
                    text = CLIPS_FAILURE_CLIPS_RESTRICTED_CATEGORY_TEXT;
                }
                break;

                case HelixClipError::UserNotAuthenticated: {
                    builder.emplace<TextElement>(
                        CLIPS_FAILURE_NOT_AUTHENTICATED_TEXT,
                        MessageElementFlag::Text, MessageColor::System);
                    builder
                        .emplace<TextElement>(LOGIN_PROMPT_TEXT,
                                              MessageElementFlag::Text,
                                              MessageColor::Link)
                        ->setLink(ACCOUNTS_LINK);
                    text = QString("%1 %2").arg(
                        CLIPS_FAILURE_NOT_AUTHENTICATED_TEXT,
                        LOGIN_PROMPT_TEXT);
                }
                break;

                // This would most likely happen if the service is down, or if the JSON payload returned has changed format
                case HelixClipError::Unknown:
                default: {
                    builder.emplace<TextElement>(
                        CLIPS_FAILURE_UNKNOWN_ERROR_TEXT.arg(errorMessage),
                        MessageElementFlag::Text, MessageColor::System);
                    text = CLIPS_FAILURE_UNKNOWN_ERROR_TEXT.arg(errorMessage);
                }
                break;
            }

            builder.message().messageText = text;
            builder.message().searchText = text;

            this->addMessage(builder.release(), MessageContext::Original);
        },
        // finallyCallback - this will always execute, so clip creation won't ever be stuck
        [this] {
            this->clipCreationTimer_.restart();
            this->isClipCreationInProgress = false;
        });
}

void TwitchChannel::deleteMessagesAs(const QString &messageID,
                                     TwitchAccount *moderator)
{
    getHelix()->deleteChatMessages(
        this->roomId(), moderator->getUserId(), messageID,
        []() {
            // Success handling, we do nothing: IRC/pubsub will dispatch the correct
            // events to update state for us.
        },
        [lifetime{this->weak_from_this()}, messageID](auto error,
                                                      const auto &message) {
            auto self =
                std::dynamic_pointer_cast<TwitchChannel>(lifetime.lock());
            if (!self)
            {
                return;
            }

            QString errorMessage = QString("Failed to delete chat messages - ");

            switch (error)
            {
                case HelixDeleteChatMessagesError::UserMissingScope: {
                    errorMessage +=
                        "Missing required scope. Re-login with your "
                        "account and try again.";
                }
                break;

                case HelixDeleteChatMessagesError::UserNotAuthorized: {
                    errorMessage +=
                        "you don't have permission to perform that action.";
                }
                break;

                case HelixDeleteChatMessagesError::MessageUnavailable: {
                    // Override default message prefix to match with IRC message format
                    errorMessage =
                        QString("The message %1 does not exist, was deleted, "
                                "or is too old to be deleted.")
                            .arg(messageID);
                }
                break;

                case HelixDeleteChatMessagesError::UserNotAuthenticated: {
                    errorMessage += "you need to re-authenticate.";
                }
                break;

                case HelixDeleteChatMessagesError::Forwarded: {
                    errorMessage += message;
                }
                break;

                case HelixDeleteChatMessagesError::Unknown:
                default: {
                    errorMessage += "An unknown error has occurred.";
                }
                break;
            }

            self->addSystemMessage(errorMessage);
        });
}

std::optional<EmotePtr> TwitchChannel::twitchBadge(const QString &set,
                                                   const QString &version) const
{
    auto badgeSets = this->badgeSets_.access();
    auto it = badgeSets->find(set);
    if (it != badgeSets->end())
    {
        auto it2 = it->second.find(version);
        if (it2 != it->second.end())
        {
            return it2->second;
        }
    }
    return std::nullopt;
}

std::vector<FfzBadges::Badge> TwitchChannel::ffzChannelBadges(
    const QString &userID) const
{
    this->tgFfzChannelBadges_.guard();

    auto it = this->ffzChannelBadges_.find(userID);
    if (it == this->ffzChannelBadges_.end())
    {
        return {};
    }

    std::vector<FfzBadges::Badge> badges;

    const auto *ffzBadges = getApp()->getFfzBadges();

    for (const auto &badgeID : it->second)
    {
        auto badge = ffzBadges->getBadge(badgeID);
        if (badge.has_value())
        {
            badges.emplace_back(*badge);
        }
    }

    return badges;
}

void TwitchChannel::setFfzChannelBadges(FfzChannelBadgeMap map)
{
    this->tgFfzChannelBadges_.guard();
    this->ffzChannelBadges_ = std::move(map);
}

std::optional<EmotePtr> TwitchChannel::ffzCustomModBadge() const
{
    return this->ffzCustomModBadge_.get();
}

std::optional<EmotePtr> TwitchChannel::ffzCustomVipBadge() const
{
    return this->ffzCustomVipBadge_.get();
}

void TwitchChannel::setFfzCustomModBadge(std::optional<EmotePtr> badge)
{
    this->ffzCustomModBadge_.set(std::move(badge));
}

void TwitchChannel::setFfzCustomVipBadge(std::optional<EmotePtr> badge)
{
    this->ffzCustomVipBadge_.set(std::move(badge));
}

std::optional<CheerEmote> TwitchChannel::cheerEmote(const QString &string) const
{
    auto sets = this->cheerEmoteSets_.access();
    for (const auto &set : *sets)
    {
        auto match = set.regex.match(string);
        if (!match.hasMatch())
        {
            continue;
        }
        QString amount = match.captured(1);
        bool ok = false;
        int bitAmount = amount.toInt(&ok);
        if (!ok)
        {
            qCDebug(chatterinoTwitch)
                << "Error parsing bit amount in cheerEmote";
        }
        for (const auto &emote : set.cheerEmotes)
        {
            if (bitAmount >= emote.minBits)
            {
                return emote;
            }
        }
    }
    return std::nullopt;
}

void TwitchChannel::updateBttvActivity()
{
    if (!getApp()->getBttvLiveUpdates())
    {
        return;
    }

    auto now = QDateTime::currentDateTimeUtc();
    if (this->nextBttvActivity_.isValid() && now < this->nextBttvActivity_)
    {
        return;
    }
    this->nextBttvActivity_ = now.addSecs(60);

    qCDebug(chatterinoBttv) << "Sending activity in" << this->getName();

    auto acc = getApp()->getAccounts()->twitch.getCurrent();
    if (acc->isAnon())
    {
        return;
    }
    getApp()->getBttvLiveUpdates()->broadcastMe(this->roomId(),
                                                acc->getUserId());
}

void TwitchChannel::updateSevenTVActivity()
{
    static const QString seventvActivityUrl =
        QStringLiteral("https://7tv.io/v3/users/%1/presences");

    const auto currentSeventvUserID =
        getApp()->getAccounts()->twitch.getCurrent()->getSeventvUserID();
    if (currentSeventvUserID.isEmpty())
    {
        return;
    }

    if (!getSettings()->enableSevenTVEventAPI ||
        !getSettings()->sendSevenTVActivity)
    {
        return;
    }

    if (this->nextSeventvActivity_.isValid() &&
        QDateTime::currentDateTimeUtc() < this->nextSeventvActivity_)
    {
        return;
    }
    // Make sure to not send activity again before receiving the response
    this->nextSeventvActivity_ = this->nextSeventvActivity_.addSecs(300);

    qCDebug(chatterinoSeventv) << "Sending activity in" << this->getName();

    getApp()->getSeventvAPI()->updatePresence(
        this->roomId(), currentSeventvUserID,
        [chan = weakOf<Channel>(this)]() {
            const auto self =
                std::dynamic_pointer_cast<TwitchChannel>(chan.lock());
            if (!self)
            {
                return;
            }
            self->nextSeventvActivity_ =
                QDateTime::currentDateTimeUtc().addSecs(60);
        },
        [](const auto &result) {
            qCDebug(chatterinoSeventv)
                << "Failed to update 7TV activity:" << result.formatError();
        });
}

void TwitchChannel::listenSevenTVCosmetics() const
{
    if (getApp()->getSeventvEventAPI())
    {
        getApp()->getSeventvEventAPI()->subscribeTwitchChannel(this->roomId());
    }
}

void TwitchChannel::syncSendWaitTimer()
{
    auto now = std::chrono::steady_clock::now();
    const auto remaining =
        this->sendWaitEnd_.has_value()
            ? std::chrono::duration_cast<std::chrono::seconds>(
                  this->sendWaitEnd_.value() - now)
            : 0s;
    if (remaining <= 0s)
    {
        this->sendWaitTimer_.stop();
        this->sendWaitUpdate.invoke("");
    }
    else
    {
        this->sendWaitUpdate.invoke(formatTime(remaining, 2));
    }
}

void TwitchChannel::setSendWait(int seconds)
{
    if (seconds <= 0)
    {
        if (this->sendWaitEnd_.has_value())
        {
            this->sendWaitEnd_ = std::nullopt;
            this->syncSendWaitTimer();
        }
        return;
    }
    this->sendWaitEnd_ =
        std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    if (!this->sendWaitTimer_.isActive())
    {
        this->sendWaitTimer_.start(1s);
        this->syncSendWaitTimer();
    }
}

bool TwitchChannel::isLoadingRecentMessages() const
{
    return this->loadingRecentMessages_.test();
}

void TwitchChannel::handlePinnedChatUpdate(const QJsonObject &data)
{
    QString type = data.value("type").toString();
    const auto innerDataValue = data.value("data");
    const auto innerData =
        innerDataValue.isObject() ? innerDataValue.toObject() : QJsonObject{};

    if (type == "pin-message" || type == "update-message")
    {
        if (!innerData.isEmpty())
        {
            if (innerData.contains("message") &&
                innerData["message"].isObject())
            {
                auto msgObj = innerData["message"].toObject();

                PinnedMessage pin;
                if (innerData.contains("id"))
                    pin.pinId = innerData["id"].toString();
                if (msgObj.contains("id"))
                    pin.messageId = msgObj["id"].toString();
                if (msgObj.contains("content") && msgObj["content"].isObject())
                {
                    auto contentObj = msgObj["content"].toObject();
                    if (contentObj.contains("text"))
                        pin.text = contentObj["text"].toString();
                }
                this->refreshPinnedMessage();
                return;
            }
        }
        this->refreshPinnedMessage();
    }
    else if (type == "unpin-message")
    {
        const auto eventPinId = pinnedChatEventPinId(innerData);
        auto currentPin = std::optional<PinnedMessage>{};
        {
            auto locked = this->currentPin_.accessConst();
            if (locked->has_value())
            {
                currentPin = **locked;
            }
        }

        if (currentPin.has_value() && !eventPinId.isEmpty() &&
            !currentPin->pinId.isEmpty() && eventPinId != currentPin->pinId)
        {
            qCDebug(chatterinoTwitch)
                << "Ignoring stale pinned chat unpin for" << this->getName()
                << "event pin:" << eventPinId
                << "current pin:" << currentPin->pinId;
            return;
        }

        if (currentPin.has_value() && getSettings()->showUnpinNotifications)
        {
            QString unpinnerName;
            if (!innerData.isEmpty())
            {
                if (innerData.contains("unpinned_by") &&
                    innerData["unpinned_by"].isObject())
                {
                    auto unpinner = innerData["unpinned_by"].toObject();
                    unpinnerName = unpinner.value("display_name").toString();
                    if (unpinnerName.isEmpty())
                    {
                        unpinnerName = unpinner.value("login").toString();
                    }
                }
            }

            if (unpinnerName.isEmpty())
            {
                this->addSystemMessage("Pinned message was removed.");
            }
            else
            {
                this->addSystemMessage(
                    QString("%1 unpinned the message.").arg(unpinnerName));
            }
        }
        this->setPinnedMessage(std::nullopt);
    }
}

SharedAccessGuard<const std::optional<TwitchChannel::PredictionEvent>>
    TwitchChannel::accessPrediction() const
{
    return this->activePrediction_.accessConst();
}

SharedAccessGuard<const std::optional<TwitchChannel::PollEvent>>
    TwitchChannel::accessPoll() const
{
    return this->activePoll_.accessConst();
}

SharedAccessGuard<const std::optional<TwitchChannel::RaidEvent>>
    TwitchChannel::accessRaid() const
{
    return this->activeRaid_.accessConst();
}

void TwitchChannel::setActivePrediction(
    std::optional<PredictionEvent> prediction)
{
    assertInGuiThread();

    this->lastPredictionUpdateAt_ = QDateTime::currentDateTimeUtc();

    {
        auto locked = this->activePrediction_.access();
        *locked = std::move(prediction);
    }
    this->predictionChanged.invoke();
}

void TwitchChannel::setActivePoll(std::optional<PollEvent> poll)
{
    assertInGuiThread();

    this->lastPollUpdateAt_ = QDateTime::currentDateTimeUtc();

    {
        auto locked = this->activePoll_.access();
        if (poll && locked->has_value() && (*locked)->id == poll->id)
        {
            const auto &previous = **locked;

            if (!poll->channelPointsVotingEnabled &&
                previous.channelPointsVotingEnabled)
            {
                poll->channelPointsVotingEnabled = true;
                poll->pointsPerVote = previous.pointsPerVote;
            }
            else if (poll->channelPointsVotingEnabled &&
                     poll->pointsPerVote <= 0 && previous.pointsPerVote > 0)
            {
                poll->pointsPerVote = previous.pointsPerVote;
            }

            if (poll->selfVotes.empty())
            {
                poll->selfVotes = previous.selfVotes;
            }
            if (!poll->createdAt.isValid())
            {
                poll->createdAt = previous.createdAt;
            }
            if (!poll->endsAt && previous.endsAt)
            {
                poll->endsAt = previous.endsAt;
            }
            if (poll->createdByName.isEmpty())
            {
                poll->createdByName = previous.createdByName;
            }
        }
        *locked = std::move(poll);
    }
    this->pollChanged.invoke();
}

void TwitchChannel::setActiveRaid(std::optional<RaidEvent> raid)
{
    assertInGuiThread();

    if (raid && raid->id != this->locallyClearedRaidId_)
    {
        this->locallyClearedRaidId_.clear();
        this->locallyClearedRaidAt_ = QDateTime();
    }

    {
        auto locked = this->activeRaid_.access();
        *locked = std::move(raid);
    }
    this->raidChanged.invoke();
}

void TwitchChannel::clearActiveRaid()
{
    assertInGuiThread();

    {
        auto locked = this->activeRaid_.access();
        if (locked->has_value() && !(**locked).id.isEmpty())
        {
            this->locallyClearedRaidId_ = (**locked).id;
            this->locallyClearedRaidAt_ = QDateTime::currentDateTimeUtc();
        }
        *locked = std::nullopt;
    }
    this->raidChanged.invoke();
}

void TwitchChannel::handlePredictionUpdate(const QJsonObject &payload)
{
    assertInGuiThread();

    QString type = payload.value("type").toString();
    auto data = payload.value("data").toObject();

    auto event = data.value("event").toObject();
    if (event.isEmpty())
    {
        event = data;
    }

    PredictionEvent prediction;
    prediction.id = event.value("id").toString();
    prediction.title = event.value("title").toString();
    prediction.status = event.value("status").toString();
    prediction.predictionWindowSeconds =
        event.value("prediction_window_seconds").toInt();
    prediction.winningOutcomeId = event.value("winning_outcome_id").toString();
    if (prediction.winningOutcomeId.isEmpty())
    {
        prediction.winningOutcomeId =
            event.value("winningOutcomeId").toString();
    }

    auto createdAtStr = event.value("created_at").toString();
    if (!createdAtStr.isEmpty())
    {
        prediction.createdAt = QDateTime::fromString(createdAtStr, Qt::ISODate);
    }

    auto lockedAtStr = event.value("locked_at").toString();
    if (!lockedAtStr.isEmpty())
    {
        prediction.lockedAt = QDateTime::fromString(lockedAtStr, Qt::ISODate);
    }

    const auto createdBy = objectFromAnyKey(event, "created_by", "createdBy");
    prediction.createdByName = userDisplayNameFromObject(createdBy);

    const auto lockedBy = objectFromAnyKey(event, "locked_by", "lockedBy");
    prediction.lockedByName = userDisplayNameFromObject(lockedBy);

    const auto endedBy = objectFromAnyKey(event, "ended_by", "endedBy");
    prediction.endedByName = userDisplayNameFromObject(endedBy);

    auto outcomesArray = event.value("outcomes").toArray();
    int outcomeCount = outcomesArray.size();
    for (int i = 0; i < outcomeCount; ++i)
    {
        auto o = outcomesArray[i].toObject();
        PredictionOutcome outcome;
        outcome.id = o.value("id").toString();
        outcome.title = o.value("title").toString();
        outcome.totalPoints = o.value("total_points").toInteger();
        outcome.totalUsers = o.value("total_users").toInt();

        if (outcomeCount == 2)
        {
            outcome.color = (i == 0) ? "BLUE" : "PINK";
        }
        else if (outcomeCount == 3)
        {
            if (i == 0)
                outcome.color = "BLUE";
            else if (i == 1)
                outcome.color = "PINK";
            else
                outcome.color = "GREEN";
        }
        else
        {
            outcome.color = "BLUE";
        }

        // Override with server-provided color if available
        auto serverColor = o.value("color").toString();
        if (!serverColor.isEmpty())
        {
            outcome.color = serverColor;
        }

        // Parse top predictors (for crown icon)
        auto topPredictors = o.value("top_predictors").toArray();
        if (!topPredictors.isEmpty())
        {
            auto top = topPredictors[0].toObject();
            outcome.topPoints = top.value("points").toVariant().toLongLong();
            outcome.topPredictorName =
                top.value("user_display_name").toString();
            if (outcome.topPredictorName.isEmpty())
            {
                outcome.topPredictorName = top.value("user_name").toString();
            }
        }

        prediction.outcomes.push_back(std::move(outcome));
    }

    const auto systemMessageKind =
        predictionSystemMessageKind(type, prediction);
    const auto systemMessageKey =
        predictionSystemMessageKey(systemMessageKind, prediction);
    if (getSettings()->showPredictionSystemMessages &&
        !systemMessageKind.isEmpty() &&
        systemMessageKey != this->lastPredictionSystemMessageKey_)
    {
        this->lastPredictionSystemMessageKey_ = systemMessageKey;
        if (systemMessageKind == "created")
        {
            this->addSystemMessage(
                QString("%1 created a prediction: \"%2\"")
                    .arg(predictionActorOrFallback(prediction.createdByName),
                         prediction.title));
        }
        else if (systemMessageKind == "locked")
        {
            this->addSystemMessage(
                QString("%1 locked the prediction")
                    .arg(predictionActorOrFallback(prediction.lockedByName)));
        }
        else if (systemMessageKind == "canceled")
        {
            this->addSystemMessage(
                QString("%1 deleted and refunded the prediction")
                    .arg(predictionActorOrFallback(prediction.endedByName)));
        }
        else if (systemMessageKind == "resolved")
        {
            const auto winnerTitle = predictionWinnerTitle(prediction);
            if (winnerTitle.isEmpty())
            {
                this->addSystemMessage(QString("%1 paid out the prediction")
                                           .arg(predictionActorOrFallback(
                                               prediction.endedByName)));
            }
            else
            {
                this->addSystemMessage(
                    QString("%1 paid out the prediction: \"%2\" won")
                        .arg(predictionActorOrFallback(prediction.endedByName),
                             winnerTitle));
            }
        }
    }

    if (systemMessageKind == "canceled")
    {
        this->setActivePrediction(std::nullopt);
        return;
    }

    this->setActivePrediction(std::move(prediction));
}

void TwitchChannel::handlePollUpdate(const QJsonObject &payload)
{
    assertInGuiThread();

    const auto type = payload.value("type").toString();
    const auto data = payload.value("data").toObject();
    auto poll = data.value("poll").toObject();
    if (poll.isEmpty())
    {
        poll = data;
    }

    auto finishCurrentPoll = [this, &poll] {
        std::optional<PollEvent> currentPoll;
        {
            auto locked = this->activePoll_.accessConst();
            if (locked->has_value())
            {
                currentPoll = **locked;
            }
        }

        if (!currentPoll)
        {
            this->setActivePoll(std::nullopt);
            return;
        }

        auto status = poll.value("status").toString();
        if (status.isEmpty())
        {
            status = "COMPLETED";
        }
        currentPoll->status = status;
        currentPoll->remainingDurationMilliseconds = 0;
        currentPoll->endsAt = QDateTime::currentDateTimeUtc();
        this->setActivePoll(std::move(currentPoll));
    };

    if (poll.isEmpty())
    {
        if (type == "POLL_END")
        {
            finishCurrentPoll();
            return;
        }

        qCWarning(chatterinoTwitch)
            << "[Polls] Ignoring malformed PubSub payload for"
            << this->getName() << "type:" << type;
        return;
    }

    PollEvent event;
    event.id = poll.value("poll_id").toString();
    if (event.id.isEmpty())
    {
        event.id = poll.value("id").toString();
    }
    event.title = poll.value("title").toString();
    event.status = poll.value("status").toString();
    if (event.status.isEmpty())
    {
        event.status = (type == "POLL_END") ? "COMPLETED" : "ACTIVE";
    }
    event.remainingDurationMilliseconds =
        poll.value("remaining_duration_milliseconds")
            .toInt(poll.value("remainingDurationMilliseconds").toInt());
    event.createdAt = parseIsoDateTime(poll.value("created_at"));
    if (!event.createdAt.isValid())
    {
        event.createdAt = parseIsoDateTime(poll.value("createdAt"));
    }
    auto endsAt = parseIsoDateTime(poll.value("ends_at"));
    if (!endsAt.isValid())
    {
        endsAt = parseIsoDateTime(poll.value("endsAt"));
    }
    if (endsAt.isValid())
    {
        event.endsAt = endsAt;
        if (event.createdAt.isValid())
        {
            event.durationSeconds =
                std::max(0, int(event.createdAt.secsTo(*event.endsAt)));
        }
    }

    const auto settings = poll.value("settings").toObject();
    const auto pointsVotes =
        settings.value("channel_points_votes").toObject().isEmpty()
            ? settings.value("communityPointsVotes").toObject()
            : settings.value("channel_points_votes").toObject();
    event.channelPointsVotingEnabled =
        pointsVotes.value("isEnabled")
            .toBool(pointsVotes.value("is_enabled").toBool());
    event.pointsPerVote = pointsVotes.value("cost").toInt(
        pointsVotes.value("community_points_cost").toInt());

    const auto createdBy = poll.value("created_by").toObject().isEmpty()
                               ? poll.value("createdBy").toObject()
                               : poll.value("created_by").toObject();
    event.createdByName = userDisplayNameFromObject(createdBy);

    const auto topContributor =
        poll.value("top_channel_points_contributor").toObject();
    const auto topContributorName = userDisplayNameFromObject(topContributor);
    const auto topContribution =
        topContributor.value("contribution")
            .toInt(topContributor.value("amount").toInt());

    const auto choices = poll.value("choices").toArray();
    event.choices.reserve(size_t(choices.size()));
    for (const auto &choiceVal : choices)
    {
        const auto choiceObj = choiceVal.toObject();
        PollChoice choice;
        choice.id = choiceObj.value("choice_id").toString();
        if (choice.id.isEmpty())
        {
            choice.id = choiceObj.value("id").toString();
        }
        choice.title = choiceObj.value("title").toString();

        const auto votesObj = choiceObj.value("votes").toObject();
        choice.totalVotes = votesObj.value("total").toInt();
        choice.freeVotes = votesObj.value("base").toInt();
        choice.channelPointsVotes =
            votesObj.value("channel_points")
                .toInt(votesObj.value("communityPoints").toInt());
        choice.totalVoters = choiceObj.value("total_voters")
                                 .toInt(choiceObj.value("totalVoters").toInt());
        if (!topContributorName.isEmpty() && choice.channelPointsVotes > 0)
        {
            choice.topChannelPointsContribution = topContribution;
            choice.topChannelPointsContributorName = topContributorName;
        }

        event.totalVotes += choice.totalVotes;
        event.choices.push_back(std::move(choice));
    }

    if (event.id.isEmpty() || event.title.isEmpty() || event.choices.empty())
    {
        if (type == "POLL_END")
        {
            finishCurrentPoll();
            return;
        }

        qCWarning(chatterinoTwitch)
            << "[Polls] Ignoring incomplete PubSub payload for"
            << this->getName() << "type:" << type;
        return;
    }

    if (type == "POLL_END")
    {
        event.remainingDurationMilliseconds = 0;
        if (!event.endsAt.has_value() || !event.endsAt->isValid())
        {
            event.endsAt = QDateTime::currentDateTimeUtc();
        }
    }

    this->setActivePoll(std::move(event));
}

void TwitchChannel::handleRaidUpdate(const QJsonObject &payload)
{
    assertInGuiThread();

    const auto topic = payload.value("topic").toString();
    if (topic != QStringLiteral("raid.%1").arg(this->roomId()))
    {
        return;
    }

    auto raid = payload.value("raid").toObject();
    if (raid.isEmpty())
    {
        raid = payload.value("data").toObject().value("raid").toObject();
    }
    if (raid.isEmpty())
    {
        return;
    }

    const auto sourceId =
        raid.value("source_id").toString(raid.value("sourceID").toString());
    if (!sourceId.isEmpty() && sourceId != this->roomId())
    {
        return;
    }

    RaidEvent event;
    event.id = raid.value("id").toString();
    event.sourceId = sourceId.isEmpty() ? this->roomId() : sourceId;
    event.targetId =
        raid.value("target_id").toString(raid.value("targetID").toString());
    event.targetLogin = raid.value("target_login")
                            .toString(raid.value("targetLogin").toString());
    event.targetDisplayName =
        raid.value("target_display_name")
            .toString(
                raid.value("targetDisplayName").toString(event.targetLogin));
    event.targetProfileImage =
        raid.value("target_profile_image")
            .toString(raid.value("targetProfileImage").toString());
    event.viewerCount = int(std::max<qint64>(
        0, parseJsonInteger(raid.value("viewer_count").isUndefined()
                                ? raid.value("viewerCount")
                                : raid.value("viewer_count"))));
    event.forceRaidNowSeconds =
        int(parseJsonInteger(raid.value("force_raid_now_seconds").isUndefined()
                                 ? raid.value("forceRaidNowSeconds")
                                 : raid.value("force_raid_now_seconds")));
    if (event.forceRaidNowSeconds <= 0)
    {
        event.forceRaidNowSeconds = 90;
    }

    const auto createdAt =
        parseJsonInteger(raid.value("raid_created_at").isUndefined()
                             ? raid.value("raidCreatedAt")
                             : raid.value("raid_created_at"));
    if (createdAt > 0)
    {
        event.raidCreatedAt = QDateTime::fromSecsSinceEpoch(createdAt, Qt::UTC);
    }
    event.receivedAt = QDateTime::currentDateTimeUtc();

    if (!event.raidCreatedAt.isValid())
    {
        auto locked = this->activeRaid_.accessConst();
        if (locked->has_value() && (**locked).id == event.id)
        {
            event.raidCreatedAt = (**locked).raidCreatedAt;
            if ((**locked).receivedAt.isValid())
            {
                event.receivedAt = (**locked).receivedAt;
            }
        }
    }

    if (event.id.isEmpty() || event.targetId.isEmpty())
    {
        return;
    }

    if (!this->locallyClearedRaidId_.isEmpty())
    {
        const auto elapsedMs = this->locallyClearedRaidAt_.isValid()
                                   ? this->locallyClearedRaidAt_.msecsTo(
                                         QDateTime::currentDateTimeUtc())
                                   : LOCALLY_CLEARED_RAID_SUPPRESSION_MS;
        if (event.id == this->locallyClearedRaidId_ && elapsedMs >= 0 &&
            elapsedMs < LOCALLY_CLEARED_RAID_SUPPRESSION_MS)
        {
            return;
        }

        this->locallyClearedRaidId_.clear();
        this->locallyClearedRaidAt_ = QDateTime();
    }

    this->setActiveRaid(std::move(event));
}

void TwitchChannel::handleUserPointsUpdate(const QJsonObject &payload)
{
    assertInGuiThread();

    const QString type = payload.value("type").toString();

    if (type != "points-earned" && type != "points-spent")
    {
        return;
    }

    const auto dataValue = payload.value("data");
    if (!dataValue.isObject())
    {
        qCWarning(chatterinoTwitch)
            << "[Points] Ignoring malformed PubSub payload for"
            << this->getName() << "type:" << type << "missing data object";
        return;
    }

    const auto data = dataValue.toObject();
    const auto balanceValue = data.value("balance");
    if (!balanceValue.isObject())
    {
        qCWarning(chatterinoTwitch)
            << "[Points] Ignoring malformed PubSub payload for"
            << this->getName() << "type:" << type << "missing balance object";
        return;
    }

    const auto balanceObj = balanceValue.toObject();
    QString channelId = balanceObj.value("channel_id").toString();
    if (channelId.isEmpty())
    {
        channelId = data.value("channel_id").toString();
    }
    if (channelId.isEmpty())
    {
        qCWarning(chatterinoTwitch)
            << "[Points] Ignoring PubSub payload for" << this->getName()
            << "type:" << type << "missing channel id";
        return;
    }
    if (channelId != this->roomId())
    {
        return;
    }

    const auto balanceField = balanceObj.value("balance");
    if (!balanceField.isDouble())
    {
        qCWarning(chatterinoTwitch)
            << "[Points] Ignoring malformed PubSub payload for"
            << this->getName() << "type:" << type << "missing numeric balance";
        return;
    }

    const qint64 newBalance = balanceField.toInteger();

    if (this->channelPoints_.load() == newBalance)
    {
        return;
    }

    qCDebug(chatterinoTwitch) << "[Points] PubSub update for" << this->getName()
                              << "type:" << type << "balance:" << newBalance;

    this->setChannelPointBalance(newBalance);
}

void TwitchChannel::refreshActivePrediction()
{
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!account)
    {
        return;
    }

    auto auth = MoltorinoAuth::resolveReadToken();
    if (!auth.hasToken())
    {
        qCDebug(chatterinoTwitch)
            << "[Predictions] Skipping active prediction fetch for"
            << this->getName() << "because no auth token is available";
        return;
    }

    if (this->predictionFetchInFlight_.exchange(true))
    {
        return;
    }

    const auto weak = this->weak_from_this();
    this->lastPredictionRefreshAt_ = QDateTime::currentDateTimeUtc();

    qCDebug(chatterinoTwitch)
        << "[Predictions] Fetching active prediction for" << this->getName();

    TwitchGql::getActivePrediction(
        this->getName(), auth.token,
        [weak](std::optional<PredictionEvent> prediction) {
            if (auto shared =
                    std::dynamic_pointer_cast<TwitchChannel>(weak.lock()))
            {
                shared->predictionFetchInFlight_.store(false);
            }
            runInGuiThread([weak,
                            prediction = std::move(prediction)]() mutable {
                auto shared =
                    std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
                if (!shared)
                {
                    return;
                }

                shared->lastPredictionUpdateAt_ =
                    QDateTime::currentDateTimeUtc();
                if (prediction)
                {
                    {
                        auto cur = shared->activePrediction_.access();
                        if (cur->has_value())
                        {
                            if ((*cur)->id == prediction->id &&
                                (*cur)->selfPoints > 0)
                            {
                                prediction->selfPoints = (*cur)->selfPoints;
                                prediction->selfOutcomeId =
                                    (*cur)->selfOutcomeId;
                            }
                        }
                    }

                    qCDebug(chatterinoTwitch)
                        << "[Predictions] Got active prediction:"
                        << prediction->title << "status:" << prediction->status;
                }
                else
                {
                    qCDebug(chatterinoTwitch)
                        << "[Predictions] No active prediction for"
                        << shared->getName();
                }
                shared->setActivePrediction(std::move(prediction));
            });
        },
        [weak](const QString &error) {
            auto shared = std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
            if (!shared)
            {
                return;
            }

            shared->predictionFetchInFlight_.store(false);
            qCDebug(chatterinoTwitch)
                << "[Predictions] Failed to fetch active prediction for"
                << shared->getName() << ":" << error;
        });
}

void TwitchChannel::refreshPrediction(bool force)
{
    if (!getSettings()->enablePredictions)
    {
        return;
    }

    if (this->roomId().isEmpty())
    {
        return;
    }

    if (this->predictionFetchInFlight_.load())
    {
        return;
    }

    const auto now = QDateTime::currentDateTimeUtc();

    if (!force && this->lastPredictionUpdateAt_.isValid() &&
        this->lastPredictionUpdateAt_.msecsTo(now) < PREDICTION_STALE_AFTER_MS)
    {
        return;
    }

    if (!force && this->lastPredictionRefreshAt_.isValid() &&
        this->lastPredictionRefreshAt_.msecsTo(now) <
            PREDICTION_MIN_REFRESH_INTERVAL_MS)
    {
        return;
    }

    this->refreshActivePrediction();
}

void TwitchChannel::refreshActivePoll()
{
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!account)
    {
        return;
    }

    auto auth = MoltorinoAuth::resolveReadToken();
    if (!auth.hasToken())
    {
        qCDebug(chatterinoTwitch)
            << "[Polls] Skipping active poll fetch for" << this->getName()
            << "because no auth token is available";
        return;
    }

    if (this->pollFetchInFlight_.exchange(true))
    {
        return;
    }

    this->lastPollRefreshAt_ = QDateTime::currentDateTimeUtc();

    const auto weak = this->weak_from_this();
    TwitchGql::getActivePoll(
        this->getName(), auth.token,
        [weak](std::optional<PollEvent> poll) {
            runInGuiThread([weak, poll = std::move(poll)]() mutable {
                auto shared =
                    std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
                if (!shared)
                {
                    return;
                }

                shared->pollFetchInFlight_.store(false);
                shared->lastPollUpdateAt_ = QDateTime::currentDateTimeUtc();
                shared->setActivePoll(std::move(poll));
            });
        },
        [weak](const QString &error) {
            auto shared = std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
            if (!shared)
            {
                return;
            }

            shared->pollFetchInFlight_.store(false);
            qCDebug(chatterinoTwitch)
                << "[Polls] Failed to fetch active poll for"
                << shared->getName() << ':' << error;
        });
}

void TwitchChannel::refreshPollIfStale(bool force)
{
    if (!getSettings()->enablePolls)
    {
        return;
    }

    if (this->roomId().isEmpty())
    {
        return;
    }

    if (this->pollFetchInFlight_.load())
    {
        return;
    }

    const auto now = QDateTime::currentDateTimeUtc();

    if (!force && this->lastPollUpdateAt_.isValid() &&
        this->lastPollUpdateAt_.msecsTo(now) < POLL_STALE_AFTER_MS)
    {
        return;
    }

    if (!force && this->lastPollRefreshAt_.isValid() &&
        this->lastPollRefreshAt_.msecsTo(now) < POLL_MIN_REFRESH_INTERVAL_MS)
    {
        return;
    }

    this->refreshActivePoll();
}

void TwitchChannel::handleChatWarningPubSub(const QJsonObject &payload)
{
    assertInGuiThread();

    const auto data = payload.value("data").toObject();
    const auto channelId = data.value("channel_id").toString();
    if (channelId.isEmpty() || channelId != this->roomId())
    {
        return;
    }

    auto account = getApp()->getAccounts()->twitch.getCurrent();
    const auto targetId = data.value("target_id").toString();
    const auto auth = MoltorinoAuth::resolveCurrentUserToken();
    const auto expectedTargetId =
        auth.userId.isEmpty() ? (account ? account->getUserId() : QString())
                              : auth.userId;
    if (!targetId.isEmpty() &&
        (expectedTargetId.isEmpty() || targetId != expectedTargetId))
    {
        return;
    }

    const auto action = data.value("action").toString();
    if (action == "acknowledge_warning")
    {
        this->clearChatWarning();
        return;
    }

    if (action != "warn")
    {
        return;
    }

    ChatWarning warning;
    warning.id = data.value("id").toString();
    warning.channelId = channelId;
    warning.reason = data.value("reason").toString();
    warning.createdAt = QDateTime::currentDateTimeUtc();

    this->setActiveChatWarning(std::move(warning), true);
}

void TwitchChannel::handleChatWarningNotice()
{
    this->showPendingChatWarningIfVisible();

    auto auth = MoltorinoAuth::resolveCurrentUserToken();
    if (!auth.hasToken())
    {
        this->showChatWarningAuthFallback();
        return;
    }

    this->refreshChatWarningIfStale(true, true);
}

void TwitchChannel::refreshChatWarningIfStale(bool force, bool notifyOnError)
{
    if (this->roomId().isEmpty())
    {
        return;
    }

    if (this->chatWarningFetchInFlight_.load())
    {
        if (notifyOnError)
        {
            this->chatWarningFetchNotifyOnError_.store(true);
        }
        return;
    }

    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!account || account->isAnon() || account->getUserId().isEmpty())
    {
        return;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    if (!force && this->lastChatWarningUpdateAt_.isValid() &&
        this->lastChatWarningUpdateAt_.msecsTo(now) <
            CHAT_WARNING_STALE_AFTER_MS)
    {
        return;
    }

    if (!force && this->lastChatWarningRefreshAt_.isValid() &&
        this->lastChatWarningRefreshAt_.msecsTo(now) <
            CHAT_WARNING_MIN_REFRESH_INTERVAL_MS)
    {
        return;
    }

    auto auth = MoltorinoAuth::resolveCurrentUserToken();
    if (!auth.hasToken())
    {
        this->lastChatWarningRefreshAt_ = now;
        if (notifyOnError)
        {
            this->showChatWarningAuthFallback();
        }
        return;
    }

    if (this->chatWarningFetchInFlight_.exchange(true))
    {
        if (notifyOnError)
        {
            this->chatWarningFetchNotifyOnError_.store(true);
        }
        return;
    }
    this->chatWarningFetchNotifyOnError_.store(notifyOnError);
    this->lastChatWarningRefreshAt_ = now;

    const auto weak = this->weak_from_this();
    const auto targetUserId =
        auth.userId.isEmpty() ? account->getUserId() : auth.userId;
    TwitchGql::getChatWarningStatus(
        this->roomId(), targetUserId, auth.token,
        [weak](std::optional<ChatWarning> warning) mutable {
            runInGuiThread([weak, warning = std::move(warning)]() mutable {
                auto shared =
                    std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
                if (!shared)
                {
                    return;
                }

                shared->chatWarningFetchInFlight_.store(false);
                const bool shouldNotify =
                    shared->chatWarningFetchNotifyOnError_.exchange(false);
                shared->lastChatWarningUpdateAt_ =
                    QDateTime::currentDateTimeUtc();
                if (warning)
                {
                    if (warning->channelId.isEmpty())
                    {
                        warning->channelId = shared->roomId();
                    }
                    shared->setActiveChatWarning(std::move(warning), true);
                }
                else
                {
                    shared->clearChatWarning();
                    if (shouldNotify)
                    {
                        shared->addSystemMessage(
                            chatWarningMissingDetailsText(shared->getName()));
                    }
                }
            });
        },
        [weak, notifyOnError](const QString &error) {
            runInGuiThread([weak, notifyOnError, error] {
                auto shared =
                    std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
                if (!shared)
                {
                    return;
                }

                shared->chatWarningFetchInFlight_.store(false);
                const bool shouldNotify =
                    notifyOnError ||
                    shared->chatWarningFetchNotifyOnError_.exchange(false);
                if (shouldNotify)
                {
                    shared->addSystemMessage(
                        "Failed to load warning: " +
                        normalizeMoltorinoAuthError("viewing Twitch warnings",
                                                    error));
                }
            });
        });
}

void TwitchChannel::acknowledgeChatWarning()
{
    if (this->roomId().isEmpty())
    {
        return;
    }

    auto auth = MoltorinoAuth::resolveCurrentUserToken();
    if (!auth.hasToken())
    {
        this->showChatWarningAuthFallback();
        return;
    }

    if (this->chatWarningAckInFlight_.exchange(true))
    {
        return;
    }

    if (!this->shownChatWarningMessageId_.isEmpty())
    {
        this->disableMessage(this->shownChatWarningMessageId_);
        getApp()->getWindows()->repaintVisibleChatWidgets(this);
    }

    const auto weak = this->weak_from_this();
    TwitchGql::acknowledgeChatWarning(
        this->roomId(), auth.token,
        [weak] {
            runInGuiThread([weak] {
                auto shared =
                    std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
                if (!shared)
                {
                    return;
                }

                shared->chatWarningAckInFlight_.store(false);
                shared->clearChatWarning();
            });
        },
        [weak](const QString &error) {
            runInGuiThread([weak, error] {
                auto shared =
                    std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
                if (!shared)
                {
                    return;
                }

                shared->chatWarningAckInFlight_.store(false);
                shared->addSystemMessage(
                    "Failed to acknowledge warning: " +
                    normalizeMoltorinoAuthError("acknowledging Twitch warnings",
                                                error));
                shared->showPendingChatWarningIfVisible();
            });
        });
}

void TwitchChannel::showPendingChatWarningIfVisible()
{
    assertInGuiThread();

    if (this->chatWarningAckInFlight_.load())
    {
        return;
    }

    if (!isChannelCurrentlyVisible(*this))
    {
        return;
    }

    std::optional<ChatWarning> warning;
    {
        auto locked = this->activeChatWarning_.accessConst();
        if (!locked->has_value())
        {
            return;
        }
        warning = **locked;
    }

    this->showChatWarningMessage(*warning);
}

void TwitchChannel::setActiveChatWarning(std::optional<ChatWarning> warning,
                                         bool showIfVisible)
{
    assertInGuiThread();

    if (warning)
    {
        if (warning->channelId.isEmpty())
        {
            warning->channelId = this->roomId();
        }
        warning->reason = sanitizeChatWarningReason(warning->reason);
        if (!warning->createdAt.isValid())
        {
            warning->createdAt = QDateTime::currentDateTimeUtc();
        }
    }

    {
        auto locked = this->activeChatWarning_.access();
        *locked = std::move(warning);
    }
    this->lastChatWarningUpdateAt_ = QDateTime::currentDateTimeUtc();

    if (showIfVisible)
    {
        this->showPendingChatWarningIfVisible();
    }
}

void TwitchChannel::clearChatWarning()
{
    assertInGuiThread();

    {
        auto locked = this->activeChatWarning_.access();
        *locked = std::nullopt;
    }
    this->lastChatWarningUpdateAt_ = QDateTime::currentDateTimeUtc();

    if (!this->shownChatWarningMessageId_.isEmpty())
    {
        this->disableMessage(this->shownChatWarningMessageId_);
        this->shownChatWarningMessageId_.clear();
        getApp()->getWindows()->repaintVisibleChatWidgets(this);
    }
}

void TwitchChannel::showChatWarningMessage(const ChatWarning &warning)
{
    assertInGuiThread();

    const auto messageId = chatWarningMessageId(warning);
    if (messageId == this->shownChatWarningMessageId_)
    {
        auto existing = this->findMessageByID(messageId);
        if (existing && !existing->flags.has(MessageFlag::Disabled))
        {
            getApp()->getWindows()->scrollToMessage(existing);
            return;
        }

        this->shownChatWarningMessageId_.clear();
    }

    if (!this->shownChatWarningMessageId_.isEmpty())
    {
        this->disableMessage(this->shownChatWarningMessageId_);
        getApp()->getWindows()->repaintVisibleChatWidgets(this);
    }

    this->addMessage(makeChatWarningMessage(*this, warning),
                     MessageContext::Original);
    this->shownChatWarningMessageId_ = messageId;
}

void TwitchChannel::showChatWarningAuthFallback()
{
    assertInGuiThread();

    const auto now = QDateTime::currentDateTimeUtc();
    if (this->lastChatWarningAuthPromptAt_.isValid() &&
        this->lastChatWarningAuthPromptAt_.msecsTo(now) <
            CHAT_WARNING_AUTH_PROMPT_COOLDOWN_MS)
    {
        return;
    }

    this->lastChatWarningAuthPromptAt_ = now;
    this->addSystemMessage(chatWarningAuthFallbackText(this->getName()));
}

void TwitchChannel::refreshChannelPointsIfStale(bool force)
{
    if (this->roomId().isEmpty())
    {
        return;
    }

    this->refreshLeadModStatus();

    if (this->channelPointsFetchInFlight_.load())
    {
        return;
    }

    const auto now = QDateTime::currentDateTimeUtc();

    if (!force && this->lastChannelPointsUpdateAt_.isValid() &&
        this->lastChannelPointsUpdateAt_.msecsTo(now) <
            CHANNEL_POINTS_STALE_AFTER_MS)
    {
        return;
    }

    if (!force && this->lastChannelPointsRefreshAt_.isValid() &&
        this->lastChannelPointsRefreshAt_.msecsTo(now) <
            CHANNEL_POINTS_MIN_REFRESH_INTERVAL_MS)
    {
        return;
    }

    this->refreshChannelPoints();
}

void TwitchChannel::refreshChannelPoints()
{
    const auto now = QDateTime::currentDateTimeUtc();
    auto clearUnavailablePoints = [this, now](bool resetRefreshGate = false,
                                              bool forceSignal = false) {
        this->lastChannelPointsRefreshAt_ = now;
        const bool hadError = !this->lastChannelPointsError_.isEmpty();
        this->lastChannelPointsError_.clear();
        const auto previousBalance = this->channelPoints_.load();
        this->setChannelPointBalance(-1);
        if (resetRefreshGate)
        {
            this->lastChannelPointsRefreshAt_ = QDateTime();
            this->lastChannelPointsUpdateAt_ = QDateTime();
        }
        if (forceSignal || (previousBalance == -1 && hadError))
        {
            this->channelPointsChanged.invoke();
        }
    };

    auto auth = MoltorinoAuth::resolveCurrentUserToken();
    if (!auth.hasToken())
    {
        clearUnavailablePoints();
        qCDebug(chatterinoTwitch)
            << "[Points] Skipping points fetch for" << this->getName()
            << "because no matching Moltorino auth token is set";
        return;
    }

    if (!authTokenHasKnownIdentity(auth) && this->isBroadcaster())
    {
        const bool wasInFlight =
            this->channelPointsFetchInFlight_.exchange(false);
        clearUnavailablePoints(true, wasInFlight);
        return;
    }

    if (authTokenBelongsToChannel(auth, *this))
    {
        const bool wasInFlight =
            this->channelPointsFetchInFlight_.exchange(false);
        clearUnavailablePoints(true, wasInFlight);
        return;
    }

    if (this->roomId().isEmpty())
    {
        return;
    }

    if (this->channelPointsFetchInFlight_.exchange(true))
    {
        return;
    }

    this->channelPointsChanged.invoke();

    this->lastChannelPointsRefreshAt_ = now;

    const auto weak = this->weak_from_this();
    const auto requestToken = auth.token;

    TwitchGql::getChannelPoints(
        this->getName(), auth.token,
        [weak, requestToken](qint64 points) {
            runInGuiThread([weak, requestToken, points]() {
                auto shared = weak.lock();
                if (!shared)
                {
                    return;
                }
                auto *channel = dynamic_cast<TwitchChannel *>(shared.get());
                if (!channel)
                {
                    return;
                }
                channel->channelPointsFetchInFlight_.store(false);
                const auto currentAuth =
                    MoltorinoAuth::resolveCurrentUserToken();
                if (!currentAuth.hasToken() ||
                    currentAuth.token != requestToken)
                {
                    channel->refreshChannelPointsIfStale(true);
                    return;
                }

                /*
                qCDebug(chatterinoTwitch)
                    << "[Points] Got" << points << "channel points for"
                    << channel->getName();
                */
                channel->setChannelPointBalance(points);
                channel->lastChannelPointsError_.clear();
                channel->channelPointsChanged.invoke();
            });
        },
        [weak, requestToken](const QString &error) {
            runInGuiThread([weak, requestToken, error]() {
                auto shared = weak.lock();
                if (!shared)
                {
                    return;
                }
                auto *channel = dynamic_cast<TwitchChannel *>(shared.get());
                if (!channel)
                {
                    return;
                }
                channel->channelPointsFetchInFlight_.store(false);
                const auto currentAuth =
                    MoltorinoAuth::resolveCurrentUserToken();
                if (!currentAuth.hasToken() ||
                    currentAuth.token != requestToken)
                {
                    channel->refreshChannelPointsIfStale(true);
                    return;
                }

                channel->lastChannelPointsError_ = normalizeMoltorinoAuthError(
                    "loading channel points", error);
                qCWarning(chatterinoTwitch)
                    << "[Points] Failed to fetch points for"
                    << channel->getName() << ':' << error;
                channel->channelPointsChanged.invoke();
            });
        });
}

qint64 TwitchChannel::channelPointBalance() const
{
    return this->channelPoints_.load();
}

bool TwitchChannel::isChannelPointsFetchInFlight() const
{
    return this->channelPointsFetchInFlight_.load();
}

bool TwitchChannel::shouldShowChannelPoints() const
{
    const auto auth = MoltorinoAuth::resolveCurrentUserToken();
    if (auth.hasToken() && authTokenHasKnownIdentity(auth))
    {
        return !authTokenBelongsToChannel(auth, *this);
    }

    return !this->isBroadcaster();
}

void TwitchChannel::setChannelPointBalance(qint64 balance)
{
    this->lastChannelPointsUpdateAt_ = QDateTime::currentDateTimeUtc();

    const qint64 previousBalance = this->channelPoints_.exchange(balance);
    if (previousBalance != balance)
    {
        this->channelPointsChanged.invoke();
    }
}

}  // namespace chatterino

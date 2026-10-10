#include "providers/twitch/ChannelManagement.hpp"

#include "Application.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "util/PostToThread.hpp"

#include <boost/signals2/connection.hpp>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace chatterino {

namespace {

constexpr int DEFAULT_COMMERCIAL_COOLDOWN_SECONDS = 8 * 60;

struct ResolvedAccess {
    ChannelManagementAccess kind = ChannelManagementAccess::Broadcaster;
    MoltorinoAuthToken gqlAuth;
};

struct PendingUpdate {
    bool active = true;
    boost::signals2::scoped_connection accountChanged;

    PendingUpdate()
        : accountChanged(
              getApp()->getAccounts()->twitch.currentUserChanged.connect(
                  [this] {
                      this->active = false;
                  }))
    {
    }
};

struct VerifiedEditorCache {
    std::mutex mutex;
    std::unordered_map<QString, QString> tokenByChannel;
};

struct CommercialCooldownCache {
    std::mutex mutex;
    std::unordered_map<QString, std::chrono::steady_clock::time_point>
        untilByChannel;
    std::unordered_set<QString> inFlightChannels;
};

enum class CommercialRequestGate {
    Started,
    InFlight,
    Cooldown,
};

struct CommercialRequestGateResult {
    CommercialRequestGate state = CommercialRequestGate::Started;
    int cooldownSeconds = 0;
};

VerifiedEditorCache &verifiedEditorCache()
{
    static VerifiedEditorCache cache;
    return cache;
}

CommercialCooldownCache &commercialCooldownCache()
{
    static CommercialCooldownCache cache;
    return cache;
}

void rememberCommercialCooldown(const QString &channelId, int seconds)
{
    if (channelId.isEmpty() || seconds <= 0)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    auto &cache = commercialCooldownCache();
    const std::lock_guard lock(cache.mutex);
    std::erase_if(cache.untilByChannel, [now](const auto &entry) {
        return entry.second <= now;
    });
    cache.untilByChannel.insert_or_assign(channelId,
                                          now + std::chrono::seconds(seconds));
}

CommercialRequestGateResult beginCommercialRequest(const QString &channelId)
{
    const auto now = std::chrono::steady_clock::now();
    auto &cache = commercialCooldownCache();
    const std::lock_guard lock(cache.mutex);

    const auto cooldown = cache.untilByChannel.find(channelId);
    if (cooldown != cache.untilByChannel.end())
    {
        if (cooldown->second > now)
        {
            const auto milliseconds =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    cooldown->second - now)
                    .count();
            const auto remaining =
                std::max<int64_t>(1, (milliseconds + 999) / 1000);
            return {
                .state = CommercialRequestGate::Cooldown,
                .cooldownSeconds = static_cast<int>(remaining),
            };
        }
        cache.untilByChannel.erase(cooldown);
    }

    if (!cache.inFlightChannels.insert(channelId).second)
    {
        return {.state = CommercialRequestGate::InFlight};
    }
    return {};
}

void finishCommercialRequest(const QString &channelId)
{
    auto &cache = commercialCooldownCache();
    const std::lock_guard lock(cache.mutex);
    cache.inFlightChannels.erase(channelId);
}

void rememberVerifiedEditor(const QString &channelId, const QString &token,
                            const QString &channelLogin)
{
    if (channelId.isEmpty() || token.isEmpty())
    {
        return;
    }
    {
        auto &cache = verifiedEditorCache();
        const std::lock_guard lock(cache.mutex);
        if (cache.tokenByChannel.size() >= 256 &&
            !cache.tokenByChannel.contains(channelId))
        {
            cache.tokenByChannel.clear();
        }
        cache.tokenByChannel.insert_or_assign(channelId, token);
    }
    MoltorinoAuth::rememberEditorChannel(
        token,
        {.id = channelId, .login = channelLogin, .displayName = channelLogin});
}

void forgetVerifiedEditor(const QString &channelId, const QString &token,
                          const QString &channelLogin)
{
    if (channelId.isEmpty() || token.isEmpty())
    {
        return;
    }
    {
        auto &cache = verifiedEditorCache();
        const std::lock_guard lock(cache.mutex);
        const auto it = cache.tokenByChannel.find(channelId);
        if (it != cache.tokenByChannel.end() && it->second == token)
        {
            cache.tokenByChannel.erase(it);
        }
    }
    MoltorinoAuth::forgetEditorChannel(token, channelId, channelLogin);
}

bool accountMatches(const MoltorinoAuthAccount &account, const QString &userId,
                    const QString &login)
{
    if (!userId.isEmpty() && !account.userId.isEmpty())
    {
        return account.userId == userId;
    }
    return !login.isEmpty() &&
           account.login.compare(login, Qt::CaseInsensitive) == 0;
}

bool accountIsStillAvailable(const MoltorinoAuthAccount &candidate)
{
    for (const auto &account : MoltorinoAuth::accounts())
    {
        if (!account.valid || account.token.trimmed().isEmpty())
        {
            continue;
        }

        if (account.token == candidate.token &&
            accountMatches(account, candidate.userId, candidate.login))
        {
            return true;
        }
    }

    return false;
}

bool gqlAuthIsStillAvailable(const MoltorinoAuthToken &candidate)
{
    for (const auto &account : MoltorinoAuth::accounts())
    {
        if (!account.valid || account.token != candidate.token)
        {
            continue;
        }
        if (accountMatches(account, candidate.userId, candidate.login))
        {
            return true;
        }
    }
    return false;
}

bool accessIsStillAvailable(const ResolvedAccess &access)
{
    if (access.kind == ChannelManagementAccess::Editor)
    {
        return gqlAuthIsStillAvailable(access.gqlAuth);
    }

    const auto current = getApp()->getAccounts()->twitch.getCurrent();
    return current && !current->isAnon() &&
           current->getUserId() == access.gqlAuth.userId &&
           current->getOAuthToken() == access.gqlAuth.token;
}

std::vector<MoltorinoAuthAccount> editorCandidates()
{
    auto candidates = MoltorinoAuth::accounts();
    std::erase_if(candidates, [](const auto &account) {
        return !account.valid || account.token.trimmed().isEmpty();
    });

    const auto current = getApp()->getAccounts()->twitch.getCurrent();
    const auto currentUserId =
        current && !current->isAnon() ? current->getUserId() : QString();
    const auto currentLogin =
        current && !current->isAnon() ? current->getUserName() : QString();

    std::stable_partition(candidates.begin(), candidates.end(),
                          [&currentUserId, &currentLogin](const auto &account) {
                              return accountMatches(account, currentUserId,
                                                    currentLogin);
                          });
    return candidates;
}

std::optional<MoltorinoAuthAccount> cachedEditorCandidate(
    const QString &channelId,
    const std::vector<MoltorinoAuthAccount> &candidates)
{
    QString cachedToken;
    {
        auto &cache = verifiedEditorCache();
        const std::lock_guard lock(cache.mutex);
        const auto it = cache.tokenByChannel.find(channelId);
        if (it != cache.tokenByChannel.end())
        {
            cachedToken = it->second;
        }
    }
    if (cachedToken.isEmpty())
    {
        return std::nullopt;
    }

    const auto candidate =
        std::find_if(candidates.begin(), candidates.end(),
                     [&cachedToken](const MoltorinoAuthAccount &account) {
                         return account.token == cachedToken;
                     });
    if (candidate != candidates.end())
    {
        return *candidate;
    }

    auto &cache = verifiedEditorCache();
    const std::lock_guard lock(cache.mutex);
    cache.tokenByChannel.erase(channelId);
    return std::nullopt;
}

QString editorAccessMessage(const QString &channelLogin)
{
    return QString("Channel management for #%1 requires the broadcaster's "
                   "Twitch account or an enabled saved editor account. Add or "
                   "refresh it in Settings -> Moltorino -> Authentication.")
        .arg(channelLogin);
}

void resolveAccess(const std::shared_ptr<TwitchChannel> &channel,
                   bool forceEditorRefresh,
                   std::function<void(ResolvedAccess)> successCallback,
                   ChannelManagement::FailureCallback failureCallback)
{
    if (!channel || channel->isEmpty())
    {
        failureCallback("Channel management requires a Twitch channel.");
        return;
    }
    if (channel->roomId().trimmed().isEmpty())
    {
        failureCallback(
            "Channel management is waiting for Twitch channel data. Try "
            "again in a moment.");
        return;
    }

    const auto current = getApp()->getAccounts()->twitch.getCurrent();
    const auto currentOwnsChannel = current && !current->isAnon() &&
                                    !current->getUserId().isEmpty() &&
                                    current->getUserId() == channel->roomId();
    if (currentOwnsChannel)
    {
        successCallback({
            .kind = ChannelManagementAccess::Broadcaster,
            .gqlAuth =
                {
                    .token = current->getOAuthToken(),
                    .userId = current->getUserId(),
                    .login = current->getUserName(),
                    .legacy = false,
                },
        });
        return;
    }

    auto candidates = editorCandidates();
    const auto savedBroadcaster =
        std::find_if(candidates.begin(), candidates.end(),
                     [&channel](const MoltorinoAuthAccount &candidate) {
                         return accountMatches(candidate, channel->roomId(),
                                               channel->getName());
                     });
    if (savedBroadcaster != candidates.end())
    {
        rememberVerifiedEditor(channel->roomId(), savedBroadcaster->token,
                               channel->getName());
        successCallback({
            .kind = ChannelManagementAccess::Editor,
            .gqlAuth =
                {
                    .token = savedBroadcaster->token,
                    .userId = savedBroadcaster->userId,
                    .login = savedBroadcaster->login,
                    .legacy = false,
                },
        });
        return;
    }

    if (!forceEditorRefresh)
    {
        if (const auto cachedEditor =
                cachedEditorCandidate(channel->roomId(), candidates))
        {
            successCallback({
                .kind = ChannelManagementAccess::Editor,
                .gqlAuth =
                    {
                        .token = cachedEditor->token,
                        .userId = cachedEditor->userId,
                        .login = cachedEditor->login,
                        .legacy = false,
                    },
            });
            return;
        }
    }

    struct ResolveState {
        std::vector<MoltorinoAuthAccount> candidates;
        size_t index = 0;
        QString channelLogin;
        QString channelId;
        QStringList errors;
        std::function<void(ResolvedAccess)> successCallback;
        ChannelManagement::FailureCallback failureCallback;
        std::shared_ptr<std::function<void()>> tryNext;
    };

    auto state = std::make_shared<ResolveState>();
    state->candidates = std::move(candidates);
    state->channelLogin = channel->getName();
    state->channelId = channel->roomId();
    state->successCallback = std::move(successCallback);
    state->failureCallback = std::move(failureCallback);

    if (state->candidates.empty())
    {
        state->failureCallback(editorAccessMessage(state->channelLogin));
        return;
    }

    state->tryNext = std::make_shared<std::function<void()>>();
    std::weak_ptr<ResolveState> weakState = state;
    *state->tryNext = [weakState] {
        auto state = weakState.lock();
        if (!state)
        {
            return;
        }

        if (state->index >= state->candidates.size())
        {
            auto keepAlive = state->tryNext;
            state->tryNext.reset();
            auto failure = std::move(state->failureCallback);
            auto message = editorAccessMessage(state->channelLogin);
            if (!state->errors.isEmpty())
            {
                message +=
                    " Last verification error: " + state->errors.constLast();
            }
            failure(message);
            return;
        }

        const auto candidate = state->candidates.at(state->index++);
        TwitchGql::getChannelEditorStatus(
            state->channelLogin, state->channelId, candidate.token,
            [state, candidate](bool isEditor) {
                if (isEditor && accountIsStillAvailable(candidate))
                {
                    rememberVerifiedEditor(state->channelId, candidate.token,
                                           state->channelLogin);
                    state->tryNext.reset();
                    auto success = std::move(state->successCallback);
                    success({
                        .kind = ChannelManagementAccess::Editor,
                        .gqlAuth =
                            {
                                .token = candidate.token,
                                .userId = candidate.userId,
                                .login = candidate.login,
                                .legacy = false,
                            },
                    });
                    return;
                }

                if (!isEditor)
                {
                    forgetVerifiedEditor(state->channelId, candidate.token,
                                         state->channelLogin);
                }

                if (state->tryNext)
                {
                    (*state->tryNext)();
                }
            },
            [state](const QString &error) {
                state->errors.push_back(error);
                if (state->tryNext)
                {
                    (*state->tryNext)();
                }
            });
    };

    (*state->tryNext)();
}

ChannelManagementMetadata fromHelixChannel(const HelixChannel &channel)
{
    ChannelManagementMetadata metadata;
    metadata.channelId = channel.userId;
    metadata.title = channel.title;
    metadata.language = channel.language;
    metadata.category.id = channel.gameId;
    metadata.category.name = channel.gameName;
    metadata.category.displayName = channel.gameName;
    metadata.tags = channel.tags;
    static const std::array DEFAULT_LABELS{
        std::pair{"DebatedSocialIssuesAndPolitics",
                  "Politics and Sensitive Social Issues"},
        std::pair{"DrugsIntoxication", "Drugs, Intoxication, or Tobacco"},
        std::pair{"Gambling", "Gambling"},
        std::pair{"MatureGame", "Mature Game"},
        std::pair{"ProfanityVulgarity", "Significant Profanity or Vulgarity"},
        std::pair{"SexualThemes", "Sexual Themes"},
        std::pair{"ViolentGraphic", "Violent and Graphic Depictions"},
    };
    for (const auto &[labelId, name] : DEFAULT_LABELS)
    {
        const auto id = QString::fromUtf8(labelId);
        metadata.contentLabels.push_back({
            .id = id,
            .name = QString::fromUtf8(name),
            .description =
                id == QStringLiteral("MatureGame")
                    ? QStringLiteral("Controlled automatically by Twitch "
                                     "from the selected category.")
                    : QString(),
            .isEnabled = channel.contentClassificationLabels.contains(id),
            .isLocked = id == QStringLiteral("MatureGame"),
            .isSelectable = id != QStringLiteral("MatureGame"),
        });
    }
    return metadata;
}

ChannelManagementMetadata fromGqlSettings(const GqlBroadcastSettings &settings)
{
    ChannelManagementMetadata metadata;
    metadata.channelId = settings.userId;
    metadata.title = settings.title;
    metadata.language = settings.language.trimmed().toLower();
    metadata.category.id = settings.category.id;
    metadata.category.name = settings.category.name;
    metadata.category.displayName = settings.category.displayName;
    metadata.tags = settings.tags;
    metadata.isRerun = settings.isRerun;
    metadata.canEditRerun = true;
    metadata.audience =
        settings.audience.compare(QStringLiteral("SUB_ONLY_LIVE"),
                                  Qt::CaseInsensitive) == 0
            ? QStringLiteral("Subscribers only")
            : QStringLiteral("Everyone");
    metadata.canEditAudience = settings.canEditAudience;
    metadata.contentLabels.reserve(settings.contentLabels.size());
    for (const auto &label : settings.contentLabels)
    {
        metadata.contentLabels.push_back({
            .id = label.id,
            .name = label.name,
            .description = label.description,
            .lockedUntil = label.lockedUntil,
            .isEnabled = label.isEnabled,
            .isLocked = label.isLocked,
            .isSelectable = label.isSelectable,
        });
    }
    return metadata;
}

QString formatHelixUpdateError(const QString &updateType,
                               HelixUpdateChannelError error,
                               const QString &message)
{
    using Error = HelixUpdateChannelError;
    auto result = QString("Failed to set %1 - ").arg(updateType);
    switch (error)
    {
        case Error::UserMissingScope:
            return result +
                   "Missing channel:manage:broadcast permission. Log in again "
                   "with the broadcaster account and try again.";
        case Error::UserNotAuthorized:
            return result + "The selected Twitch account no longer matches the "
                            "broadcaster.";
        case Error::Ratelimited:
            return result +
                   "Twitch is rate limiting updates. Try again in a few "
                   "seconds.";
        case Error::Forwarded:
            return result + message;
        case Error::Unknown:
        default:
            return result +
                   QString("An unknown error occurred (%1).").arg(message);
    }
}

QString formatHelixCommercialError(HelixStartCommercialError error,
                                   const QString &message)
{
    using Error = HelixStartCommercialError;
    const auto prefix = QStringLiteral("Failed to start commercial - ");
    switch (error)
    {
        case Error::UserMissingScope:
            return prefix +
                   "Missing channel:edit:commercial permission. Log in again "
                   "with the broadcaster account and try again.";
        case Error::TokenMustMatchBroadcaster:
            return prefix + "The selected Twitch account no longer matches the "
                            "broadcaster.";
        case Error::BroadcasterNotStreaming:
            return prefix + "The channel must be live to run a commercial.";
        case Error::MissingLengthParameter:
            return prefix + "Choose a valid commercial length.";
        case Error::Ratelimited:
            return prefix + "The commercial cooldown has not expired yet.";
        case Error::Forwarded:
            return prefix + message;
        case Error::Unknown:
        default:
            return prefix +
                   QString("An unknown error occurred (%1).").arg(message);
    }
}

QString formatGqlCommercialError(const GqlStartAdResult &result)
{
    const auto code = result.errorCode.toUpper();
    if (code == QStringLiteral("RATE_LIMITED"))
    {
        if (result.retryAfterSeconds > 0)
        {
            return QString("Commercial cooldown active. Try again in %1 "
                           "seconds.")
                .arg(result.retryAfterSeconds);
        }
        return "Commercial cooldown active. Try again shortly.";
    }
    if (code == QStringLiteral("USER_NOT_AUTHORIZED") ||
        code == QStringLiteral("UNAUTHORIZED") ||
        code == QStringLiteral("AUTH_FAILURE"))
    {
        return "The saved account no longer has editor permission to run "
               "commercials in this channel.";
    }
    if (code.contains(QStringLiteral("LIVE")) ||
        code == QStringLiteral("FAILED_PRECONDITION"))
    {
        return "The channel must be live and eligible to run a commercial.";
    }
    return QString("Twitch could not start the commercial (%1).")
        .arg(result.errorCode);
}

using MetadataUpdateTask = std::function<void(std::function<void()>)>;

struct MetadataUpdateQueueState {
    std::mutex mutex;
    std::unordered_map<QString, std::deque<MetadataUpdateTask>> queues;
};

MetadataUpdateQueueState &metadataUpdateQueueState()
{
    static MetadataUpdateQueueState state;
    return state;
}

void startNextMetadataUpdate(const QString &channelId);

void finishMetadataUpdate(const QString &channelId)
{
    bool hasNext = false;
    {
        auto &state = metadataUpdateQueueState();
        std::unique_lock lock(state.mutex);
        const auto queue = state.queues.find(channelId);
        if (queue == state.queues.end() || queue->second.empty())
        {
            return;
        }

        queue->second.pop_front();
        if (queue->second.empty())
        {
            state.queues.erase(queue);
        }
        else
        {
            hasNext = true;
        }
    }

    if (hasNext)
    {
        runInGuiThread([channelId] {
            startNextMetadataUpdate(channelId);
        });
    }
}

void startNextMetadataUpdate(const QString &channelId)
{
    MetadataUpdateTask task;
    {
        auto &state = metadataUpdateQueueState();
        std::unique_lock lock(state.mutex);
        const auto queue = state.queues.find(channelId);
        if (queue == state.queues.end() || queue->second.empty())
        {
            return;
        }
        task = queue->second.front();
    }

    auto completed = std::make_shared<std::atomic_bool>(false);
    task([channelId, completed] {
        if (!completed->exchange(true))
        {
            finishMetadataUpdate(channelId);
        }
    });
}

bool enqueueMetadataUpdate(const QString &channelId, MetadataUpdateTask task)
{
    bool shouldStart = false;
    {
        auto &state = metadataUpdateQueueState();
        std::unique_lock lock(state.mutex);
        const auto existing = state.queues.find(channelId);
        if ((existing == state.queues.end() && state.queues.size() >= 256) ||
            (existing != state.queues.end() && existing->second.size() >= 32))
        {
            return false;
        }
        auto &queue = state.queues[channelId];
        shouldStart = queue.empty();
        queue.push_back(std::move(task));
    }

    if (shouldStart)
    {
        runInGuiThread([channelId] {
            startNextMetadataUpdate(channelId);
        });
    }
    return true;
}

struct MetadataUpdateCompletion {
    std::function<void()> success;
    ChannelManagement::FailureCallback failure;
    std::function<void()> finishQueue;
    std::atomic_bool completed = false;

    void succeed()
    {
        if (this->completed.exchange(true))
        {
            return;
        }
        this->success();
        this->finishQueue();
    }

    void fail(const QString &error)
    {
        if (this->completed.exchange(true))
        {
            return;
        }
        this->failure(error);
        this->finishQueue();
    }
};

using CompositeUpdateStep = std::function<void(
    std::function<void()>, ChannelManagement::FailureCallback)>;

struct CompositeUpdateState {
    std::vector<CompositeUpdateStep> steps;
    size_t index = 0;
    std::shared_ptr<MetadataUpdateCompletion> completion;
    std::shared_ptr<std::function<void()>> advance;
};

void runCompositeUpdate(std::vector<CompositeUpdateStep> steps,
                        std::shared_ptr<MetadataUpdateCompletion> completion)
{
    if (steps.empty())
    {
        completion->succeed();
        return;
    }

    auto state = std::make_shared<CompositeUpdateState>();
    state->steps = std::move(steps);
    state->completion = std::move(completion);
    state->advance = std::make_shared<std::function<void()>>();
    std::weak_ptr<CompositeUpdateState> weakState = state;
    *state->advance = [weakState] {
        auto state = weakState.lock();
        if (!state)
        {
            return;
        }
        if (state->index >= state->steps.size())
        {
            state->advance.reset();
            state->completion->succeed();
            return;
        }

        const auto stepIndex = state->index++;
        auto step = state->steps.at(stepIndex);
        step(
            [state] {
                if (state->advance)
                {
                    (*state->advance)();
                }
            },
            [state, stepIndex](const QString &error) {
                state->advance.reset();
                auto message = error;
                if (stepIndex > 0)
                {
                    message +=
                        " Some earlier changes were saved; reload before "
                        "retrying.";
                }
                state->completion->fail(message);
            });
    };

    (*state->advance)();
}

}  // namespace

namespace ChannelManagement {

bool isValidCommercialLength(int lengthSeconds)
{
    static constexpr std::array VALID_LENGTHS{30, 60, 90, 120, 150, 180};
    return std::ranges::find(VALID_LENGTHS, lengthSeconds) !=
           VALID_LENGTHS.end();
}

int commercialCooldownRemainingSeconds(const QString &channelId)
{
    if (channelId.isEmpty())
    {
        return 0;
    }

    const auto now = std::chrono::steady_clock::now();
    auto &cache = commercialCooldownCache();
    const std::lock_guard lock(cache.mutex);
    const auto it = cache.untilByChannel.find(channelId);
    if (it == cache.untilByChannel.end())
    {
        return 0;
    }
    if (it->second <= now)
    {
        cache.untilByChannel.erase(it);
        return 0;
    }

    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(it->second - now)
            .count();
    return std::max(1, static_cast<int>((milliseconds + 999) / 1000));
}

bool isValidTag(const QString &tag)
{
    static const QRegularExpression VALID_TAG(
        QStringLiteral(R"(^[\p{L}\p{N}]{1,25}$)"),
        QRegularExpression::UseUnicodePropertiesOption);
    return VALID_TAG.match(tag).hasMatch();
}

void rankCategoryResults(std::vector<ChannelManagementCategory> &categories,
                         const QString &query)
{
    const auto rank = [&query](const ChannelManagementCategory &category) {
        const auto label =
            (category.displayName.isEmpty() ? category.name
                                            : category.displayName)
                .trimmed();
        if (label.compare(query, Qt::CaseInsensitive) == 0)
        {
            return 0;
        }
        if (label.startsWith(query, Qt::CaseInsensitive))
        {
            return 1;
        }
        if (label.contains(QStringLiteral(" ") + query, Qt::CaseInsensitive))
        {
            return 2;
        }
        if (label.contains(query, Qt::CaseInsensitive))
        {
            return 3;
        }
        return 4;
    };

    std::stable_sort(categories.begin(), categories.end(),
                     [&rank](const auto &lhs, const auto &rhs) {
                         return rank(lhs) < rank(rhs);
                     });
}

bool hasVerifiedEditorAccess(const QString &channelId)
{
    if (channelId.isEmpty())
    {
        return false;
    }

    QString cachedToken;
    {
        auto &cache = verifiedEditorCache();
        const std::lock_guard lock(cache.mutex);
        const auto it = cache.tokenByChannel.find(channelId);
        if (it != cache.tokenByChannel.end())
        {
            cachedToken = it->second;
        }
    }

    const auto accounts = MoltorinoAuth::accounts();
    if (!cachedToken.isEmpty())
    {
        const auto stillValid =
            std::ranges::any_of(accounts, [&cachedToken](const auto &account) {
                return account.valid && account.token == cachedToken;
            });
        if (stillValid)
        {
            return true;
        }

        auto &cache = verifiedEditorCache();
        const std::lock_guard lock(cache.mutex);
        cache.tokenByChannel.erase(channelId);
    }

    for (const auto &account : accounts)
    {
        if (!account.valid || account.token.isEmpty())
        {
            continue;
        }
        const auto verified = std::ranges::any_of(
            account.verifiedEditorChannels,
            [&channelId](const MoltorinoAuthChannel &channel) {
                return channel.id == channelId;
            });
        if (!verified)
        {
            continue;
        }

        auto &cache = verifiedEditorCache();
        const std::lock_guard lock(cache.mutex);
        if (cache.tokenByChannel.size() >= 256 &&
            !cache.tokenByChannel.contains(channelId))
        {
            cache.tokenByChannel.clear();
        }
        cache.tokenByChannel.insert_or_assign(channelId, account.token);
        return true;
    }
    return false;
}

void verifyAccess(const std::shared_ptr<TwitchChannel> &channel,
                  bool forceEditorRefresh,
                  std::function<void(ChannelManagementAccess)> successCallback,
                  FailureCallback failureCallback)
{
    resolveAccess(
        channel, forceEditorRefresh,
        [successCallback =
             std::move(successCallback)](ResolvedAccess access) mutable {
            successCallback(access.kind);
        },
        std::move(failureCallback));
}

void loadState(const std::shared_ptr<TwitchChannel> &channel,
               std::function<void(ChannelManagementState)> successCallback,
               FailureCallback failureCallback)
{
    resolveAccess(
        channel, false,
        [channel, successCallback = std::move(successCallback),
         failureCallback](ResolvedAccess access) mutable {
            TwitchGql::getBroadcastManagementState(
                channel->getName(), channel->roomId(), access.gqlAuth.token,
                [channel, access, successCallback,
                 failureCallback](GqlBroadcastSettings settings) mutable {
                    if (settings.userId != channel->roomId())
                    {
                        failureCallback(
                            "Twitch returned a different channel while "
                            "loading stream information.");
                        return;
                    }
                    successCallback({
                        .access = access.kind,
                        .metadata = fromGqlSettings(settings),
                    });
                },
                [channel, access, successCallback,
                 failureCallback](const QString &error) mutable {
                    if (access.kind == ChannelManagementAccess::Broadcaster)
                    {
                        getHelix()->getChannel(
                            channel->roomId(),
                            [channel,
                             successCallback = std::move(successCallback)](
                                const HelixChannel &helixChannel) mutable {
                                auto metadata = fromHelixChannel(helixChannel);
                                metadata.isRerun =
                                    channel->accessStreamStatus()->rerun;
                                successCallback({
                                    .access =
                                        ChannelManagementAccess::Broadcaster,
                                    .metadata = std::move(metadata),
                                });
                            },
                            [failureCallback] {
                                failureCallback(
                                    "Could not load stream information from "
                                    "Twitch.");
                            });
                        return;
                    }
                    failureCallback(MoltorinoAuth::normalizeAuthError(
                        "loading channel management", error));
                });
        },
        failureCallback);
}

void searchCategories(
    const QString &query,
    std::function<void(std::vector<ChannelManagementCategory>)> successCallback,
    FailureCallback failureCallback)
{
    const auto trimmed = query.trimmed();
    if (trimmed.isEmpty())
    {
        successCallback({});
        return;
    }

    getHelix()->searchGames(
        trimmed,
        [successCallback = std::move(successCallback)](
            const std::vector<HelixGame> &games) mutable {
            std::vector<ChannelManagementCategory> categories;
            categories.reserve(games.size());
            for (const auto &game : games)
            {
                if (game.id.isEmpty() || game.name.isEmpty())
                {
                    continue;
                }
                categories.push_back({
                    .id = game.id,
                    .name = game.name,
                    .displayName = game.name,
                    .boxArtUrl = game.boxArtUrl,
                });
            }
            successCallback(std::move(categories));
        },
        [failureCallback] {
            failureCallback("Failed to search Twitch categories.");
        });
}

void updateMetadata(const std::shared_ptr<TwitchChannel> &channel,
                    ChannelManagementUpdate update,
                    std::function<void()> successCallback,
                    FailureCallback failureCallback)
{
    if (update.empty())
    {
        successCallback();
        return;
    }

    if (update.title)
    {
        *update.title = update.title->trimmed();
        if (update.title->isEmpty())
        {
            failureCallback("The stream title cannot be empty.");
            return;
        }
        if (update.title->size() > 140)
        {
            failureCallback("The stream title cannot exceed 140 characters.");
            return;
        }
    }
    if (update.language)
    {
        *update.language = update.language->trimmed().toLower();
        if (update.language->isEmpty() || update.language->size() > 10)
        {
            failureCallback("Choose a valid stream language.");
            return;
        }
    }
    if (update.category && (update.category->id.trimmed().isEmpty() ||
                            update.category->name.trimmed().isEmpty()))
    {
        failureCallback("Choose a valid Twitch category.");
        return;
    }
    if (update.tags)
    {
        if (update.tags->size() > 10)
        {
            failureCallback("A channel can have at most 10 tags.");
            return;
        }
        QStringList normalizedTags;
        for (auto tag : *update.tags)
        {
            tag = tag.trimmed();
            if (!isValidTag(tag))
            {
                failureCallback(
                    QString("Tag '%1' must be 1-25 letters or numbers with "
                            "no spaces.")
                        .arg(tag));
                return;
            }
            const auto duplicate = std::ranges::any_of(
                normalizedTags, [&tag](const QString &existing) {
                    return existing.compare(tag, Qt::CaseInsensitive) == 0;
                });
            if (duplicate)
            {
                failureCallback(QString("Tag '%1' is duplicated.").arg(tag));
                return;
            }
            normalizedTags.push_back(std::move(tag));
        }
        *update.tags = std::move(normalizedTags);
    }
    if (update.contentLabels)
    {
        QStringList ids;
        for (const auto &label : *update.contentLabels)
        {
            if (label.id.trimmed().isEmpty() || ids.contains(label.id))
            {
                failureCallback(
                    "Twitch returned invalid content classification labels.");
                return;
            }
            ids.push_back(label.id);
        }
    }

    if (!channel || channel->isEmpty() || channel->roomId().trimmed().isEmpty())
    {
        failureCallback(
            "Channel management is waiting for Twitch channel data. Try "
            "again in a moment.");
        return;
    }

    const auto channelId = channel->roomId();
    auto pending = std::make_shared<PendingUpdate>();
    const auto rejected = failureCallback;
    const auto queued = enqueueMetadataUpdate(
        channelId, [channel, channelId, pending, update = std::move(update),
                    successCallback = std::move(successCallback),
                    failureCallback = std::move(failureCallback)](
                       std::function<void()> finishQueue) mutable {
            auto completion = std::make_shared<MetadataUpdateCompletion>();
            completion->success = std::move(successCallback);
            completion->failure = std::move(failureCallback);
            completion->finishQueue = std::move(finishQueue);

            if (!pending->active)
            {
                completion->fail(
                    "The Twitch account changed while the update was waiting.");
                return;
            }

            resolveAccess(
                channel, false,
                [channel, channelId, update = std::move(update), completion,
                 pending](ResolvedAccess access) mutable {
                    if (!pending->active || !accessIsStillAvailable(access))
                    {
                        completion->fail(
                            "The channel-management account changed while "
                            "the update was being prepared.");
                        return;
                    }

                    std::vector<CompositeUpdateStep> steps;
                    const bool hasCoreUpdate =
                        update.title || update.language || update.category;

                    if (access.kind == ChannelManagementAccess::Broadcaster)
                    {
                        HelixChannelUpdate helixUpdate;
                        helixUpdate.title = update.title;
                        helixUpdate.language = update.language;
                        if (update.category)
                        {
                            helixUpdate.gameId = update.category->id;
                        }
                        helixUpdate.tags = update.tags;
                        if (update.contentLabels)
                        {
                            static const QStringList WRITABLE_LABELS{
                                "DebatedSocialIssuesAndPolitics",
                                "DrugsIntoxication",
                                "Gambling",
                                "ProfanityVulgarity",
                                "SexualThemes",
                                "ViolentGraphic",
                            };
                            std::vector<HelixContentClassificationLabelState>
                                labels;
                            for (const auto &label : *update.contentLabels)
                            {
                                if (WRITABLE_LABELS.contains(label.id) &&
                                    label.isSelectable && !label.isLocked)
                                {
                                    labels.push_back(
                                        {label.id, label.isEnabled});
                                }
                            }
                            helixUpdate.contentClassificationLabels =
                                std::move(labels);
                        }

                        if (!helixUpdate.empty())
                        {
                            steps.push_back(
                                [channel, channelId, update, helixUpdate](
                                    std::function<void()> success,
                                    FailureCallback failure) {
                                    getHelix()->updateChannel(
                                        channelId, helixUpdate,
                                        [channel, update,
                                         success = std::move(success)](
                                            const auto &) mutable {
                                            if (update.title)
                                            {
                                                channel->updateStreamTitle(
                                                    *update.title);
                                            }
                                            if (update.category)
                                            {
                                                channel->updateStreamGame(
                                                    update.category->displayName
                                                            .isEmpty()
                                                        ? update.category->name
                                                        : update.category
                                                              ->displayName,
                                                    update.category->id);
                                            }
                                            success();
                                        },
                                        [failure = std::move(failure)](
                                            auto error,
                                            const auto &message) mutable {
                                            failure(formatHelixUpdateError(
                                                "stream information", error,
                                                message));
                                        });
                                });
                        }
                    }
                    else if (hasCoreUpdate)
                    {
                        steps.push_back([channel, channelId, update, access,
                                         pending](std::function<void()> success,
                                                  FailureCallback failure) {
                            TwitchGql::getBroadcastSettings(
                                channel->getName(), access.gqlAuth.token,
                                [channel, channelId, update, access, pending,
                                 success = std::move(success), failure](
                                    GqlBroadcastSettings settings) mutable {
                                    if (settings.userId != channelId)
                                    {
                                        failure("Twitch returned a different "
                                                "channel while preparing the "
                                                "update.");
                                        return;
                                    }
                                    if (!pending->active ||
                                        !accessIsStillAvailable(access))
                                    {
                                        failure("The saved editor account was "
                                                "disabled before saving.");
                                        return;
                                    }
                                    if (update.title)
                                    {
                                        settings.title = *update.title;
                                    }
                                    if (update.language)
                                    {
                                        settings.language = *update.language;
                                    }
                                    if (update.category)
                                    {
                                        settings.category.id =
                                            update.category->id;
                                        settings.category.name =
                                            update.category->name;
                                        settings.category.displayName =
                                            update.category->displayName;
                                    }
                                    TwitchGql::updateBroadcastSettings(
                                        settings, access.gqlAuth.token,
                                        [channel, channelId, update,
                                         success = std::move(success),
                                         failure](GqlBroadcastSettings
                                                      updated) mutable {
                                            if (updated.userId != channelId)
                                            {
                                                failure("Twitch returned a "
                                                        "different channel "
                                                        "after saving.");
                                                return;
                                            }
                                            if (update.title)
                                            {
                                                channel->updateStreamTitle(
                                                    *update.title);
                                            }
                                            if (update.category)
                                            {
                                                channel->updateStreamGame(
                                                    updated.category.displayName
                                                            .isEmpty()
                                                        ? updated.category.name
                                                        : updated.category
                                                              .displayName,
                                                    updated.category.id);
                                            }
                                            success();
                                        },
                                        [failure](const QString &error) {
                                            failure(MoltorinoAuth::
                                                        normalizeAuthError(
                                                            "updating "
                                                            "stream "
                                                            "information",
                                                            error));
                                        });
                                },
                                [failure](const QString &error) {
                                    failure(MoltorinoAuth::normalizeAuthError(
                                        "loading current "
                                        "stream information",
                                        error));
                                });
                        });
                    }

                    if (access.kind == ChannelManagementAccess::Editor &&
                        update.tags)
                    {
                        steps.push_back([channelId, tags = *update.tags, access,
                                         pending](std::function<void()> success,
                                                  FailureCallback failure) {
                            if (!pending->active ||
                                !accessIsStillAvailable(access))
                            {
                                failure("The saved editor account was "
                                        "disabled before saving tags.");
                                return;
                            }
                            TwitchGql::setFreeformTags(
                                channelId, tags, access.gqlAuth.token,
                                [success = std::move(success)](
                                    const QStringList &) mutable {
                                    success();
                                },
                                [failure](const QString &error) {
                                    failure(MoltorinoAuth::normalizeAuthError(
                                        "updating channel "
                                        "tags",
                                        error));
                                });
                        });
                    }

                    if (access.kind == ChannelManagementAccess::Editor &&
                        update.contentLabels)
                    {
                        QVector<GqlContentClassificationLabel> labels;
                        labels.reserve(static_cast<qsizetype>(
                            update.contentLabels->size()));
                        for (const auto &label : *update.contentLabels)
                        {
                            labels.push_back({
                                .id = label.id,
                                .name = label.name,
                                .description = label.description,
                                .lockedUntil = label.lockedUntil,
                                .isEnabled = label.isEnabled,
                                .isLocked = label.isLocked,
                                .isSelectable = label.isSelectable,
                            });
                        }
                        steps.push_back([channelId, labels = std::move(labels),
                                         access,
                                         pending](std::function<void()> success,
                                                  FailureCallback failure) {
                            if (!pending->active ||
                                !accessIsStillAvailable(access))
                            {
                                failure("The saved editor account was "
                                        "disabled before saving content "
                                        "labels.");
                                return;
                            }
                            TwitchGql::setContentClassificationLabels(
                                channelId, labels, access.gqlAuth.token,
                                [success =
                                     std::move(success)](const auto &) mutable {
                                    success();
                                },
                                [failure](const QString &error) {
                                    failure(MoltorinoAuth::normalizeAuthError(
                                        "updating content "
                                        "labels",
                                        error));
                                });
                        });
                    }

                    if (update.isRerun)
                    {
                        steps.push_back([channelId,
                                         shouldBeRerun = *update.isRerun,
                                         access,
                                         pending](std::function<void()> success,
                                                  FailureCallback failure) {
                            if (!pending->active ||
                                !accessIsStillAvailable(access))
                            {
                                failure("The channel-management account "
                                        "changed before saving rerun "
                                        "status.");
                                return;
                            }
                            TwitchGql::setChannelRerunStatus(
                                channelId, shouldBeRerun, access.gqlAuth.token,
                                [success = std::move(success)](bool) mutable {
                                    success();
                                },
                                [failure](const QString &error) {
                                    failure(MoltorinoAuth::normalizeAuthError(
                                        "updating rerun "
                                        "status",
                                        error));
                                });
                        });
                    }

                    runCompositeUpdate(std::move(steps), completion);
                },
                [completion](const QString &error) {
                    completion->fail(error);
                });
        });
    if (!queued)
    {
        rejected("Too many channel updates are waiting. Try again shortly.");
    }
}

void updateMetadata(const std::shared_ptr<TwitchChannel> &channel,
                    std::optional<QString> title,
                    std::optional<ChannelManagementCategory> category,
                    std::function<void()> successCallback,
                    FailureCallback failureCallback)
{
    ChannelManagementUpdate update;
    update.title = std::move(title);
    update.category = std::move(category);
    updateMetadata(channel, std::move(update), std::move(successCallback),
                   std::move(failureCallback));
}

void updateTitle(const std::shared_ptr<TwitchChannel> &channel,
                 const QString &title, std::function<void()> successCallback,
                 FailureCallback failureCallback)
{
    updateMetadata(channel, title, std::nullopt, std::move(successCallback),
                   std::move(failureCallback));
}

void updateCategory(const std::shared_ptr<TwitchChannel> &channel,
                    const ChannelManagementCategory &category,
                    std::function<void()> successCallback,
                    FailureCallback failureCallback)
{
    updateMetadata(channel, std::nullopt, category, std::move(successCallback),
                   std::move(failureCallback));
}

void startCommercial(
    const std::shared_ptr<TwitchChannel> &channel, int lengthSeconds,
    ChannelManagementCommercialTrigger trigger,
    std::function<void(ChannelManagementCommercialResult)> successCallback,
    std::function<void(ChannelManagementCommercialFailure)> failureCallback)
{
    if (!channel || channel->isEmpty())
    {
        failureCallback({
            .message = "The /commercial command only works in Twitch channels.",
        });
        return;
    }
    if (!isValidCommercialLength(lengthSeconds))
    {
        failureCallback({
            .message = "Commercial length must be 30, 60, 90, 120, 150, or "
                       "180 seconds.",
        });
        return;
    }
    if (!channel->isLive())
    {
        failureCallback({
            .message = "The channel must be live to run a commercial.",
        });
        return;
    }

    const auto channelId = channel->roomId();
    if (channelId.isEmpty())
    {
        failureCallback({
            .message = "Twitch has not finished loading this channel yet.",
        });
        return;
    }

    const auto gate = beginCommercialRequest(channelId);
    if (gate.state == CommercialRequestGate::Cooldown)
    {
        failureCallback({
            .message = QString("Another commercial is available in %1 seconds.")
                           .arg(gate.cooldownSeconds),
            .retryAfterSeconds = gate.cooldownSeconds,
        });
        return;
    }
    if (gate.state == CommercialRequestGate::InFlight)
    {
        failureCallback({
            .message = "A commercial request is already in progress.",
        });
        return;
    }

    resolveAccess(
        channel, false,
        [channel, channelId, lengthSeconds, trigger,
         successCallback = std::move(successCallback),
         failureCallback](ResolvedAccess access) mutable {
            if (!accessIsStillAvailable(access))
            {
                finishCommercialRequest(channelId);
                failureCallback(
                    {.message = "The channel management account changed before "
                                "the commercial could start."});
                return;
            }
            if (access.kind == ChannelManagementAccess::Broadcaster)
            {
                getHelix()->startCommercial(
                    channel->roomId(), lengthSeconds,
                    [channelId, successCallback = std::move(successCallback)](
                        const HelixStartCommercialResponse &response) mutable {
                        const auto retryAfterSeconds =
                            response.retryAfter > 0
                                ? response.retryAfter
                                : DEFAULT_COMMERCIAL_COOLDOWN_SECONDS;
                        rememberCommercialCooldown(channelId,
                                                   retryAfterSeconds);
                        finishCommercialRequest(channelId);
                        successCallback({
                            .lengthSeconds = response.length,
                            .retryAfterSeconds = retryAfterSeconds,
                        });
                    },
                    [channelId, failureCallback](auto error,
                                                 const auto &message) {
                        finishCommercialRequest(channelId);
                        failureCallback({
                            .message =
                                formatHelixCommercialError(error, message),
                        });
                    });
                return;
            }

            TwitchGql::startAd(
                channel->roomId(), lengthSeconds,
                trigger == ChannelManagementCommercialTrigger::ChatCommand
                    ? GqlStartAdTrigger::ChatCommand
                    : GqlStartAdTrigger::QuickAction,
                access.gqlAuth.token,
                [channelId, successCallback = std::move(successCallback),
                 lengthSeconds,
                 failureCallback](GqlStartAdResult result) mutable {
                    if (!result.errorCode.isEmpty())
                    {
                        rememberCommercialCooldown(channelId,
                                                   result.retryAfterSeconds);
                        finishCommercialRequest(channelId);
                        failureCallback({
                            .message = formatGqlCommercialError(result),
                            .retryAfterSeconds = result.retryAfterSeconds,
                        });
                        return;
                    }
                    const auto retryAfterSeconds =
                        result.retryAfterSeconds > 0
                            ? result.retryAfterSeconds
                            : DEFAULT_COMMERCIAL_COOLDOWN_SECONDS;
                    rememberCommercialCooldown(channelId, retryAfterSeconds);
                    finishCommercialRequest(channelId);
                    successCallback({
                        .lengthSeconds = result.lengthSeconds > 0
                                             ? result.lengthSeconds
                                             : lengthSeconds,
                        .retryAfterSeconds = retryAfterSeconds,
                    });
                },
                [channelId, failureCallback](const QString &error) {
                    finishCommercialRequest(channelId);
                    failureCallback({
                        .message = MoltorinoAuth::normalizeAuthError(
                            "running a commercial", error),
                    });
                });
        },
        [channelId, failureCallback](const QString &error) {
            finishCommercialRequest(channelId);
            failureCallback({.message = error});
        });
}

}  // namespace ChannelManagement

}  // namespace chatterino

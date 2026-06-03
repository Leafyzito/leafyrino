#include "controllers/commands/builtin/twitch/Nuke.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "messages/Message.hpp"
#include "messages/MessageFlag.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "util/Helpers.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QDateTime>
#include <QHash>
#include <QProcess>
#include <QSet>
#include <QStringBuilder>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

namespace {

using namespace chatterino;

constexpr int MAX_SPAM_COUNT = 100;
constexpr int DEFAULT_SPAM_INTERVAL_MS = 30;
constexpr int MIN_SPAM_INTERVAL_MS = 10;
constexpr int MAX_SPAM_INTERVAL_MS = 5000;
constexpr int NORMAL_CHATTER_PYRAMID_INTERVAL_MS = 1200;
constexpr int MAX_PYRAMID_HEIGHT = (MAX_SPAM_COUNT + 1) / 2;
constexpr int MAX_CHAT_MESSAGE_LENGTH = 500;
constexpr int DUPLICATE_BYPASS_SUFFIX_RESERVE = 4;
constexpr int MAX_PYRAMID_ROW_MESSAGE_LENGTH =
    MAX_CHAT_MESSAGE_LENGTH - DUPLICATE_BYPASS_SUFFIX_RESERVE;
constexpr int SPAM_FINISH_CHECK_MS = 1000;
constexpr int SPAM_FINISH_MAX_WAIT_MS = 10000;
constexpr int MAX_TIMEOUT_SECONDS = 14 * 24 * 60 * 60;
constexpr int MAX_NUKE_RANGE_SECONDS = 10 * 60;
constexpr int NUKE_DELETE_INTERVAL_MS = 80;
constexpr int NUKE_MODERATION_INTERVAL_MS = 150;
constexpr int NUKE_RATE_LIMIT_BACKOFF_MS = 1200;

enum class NukeAction {
    Timeout,
    Ban,
    Delete,
};

struct ParseResult {
    bool isNuke = false;
    bool isStop = false;
    bool complete = false;
    QString error;

    QString targetText;
    bool exact = false;
    NukeAction action = NukeAction::Timeout;
    int timeoutSeconds = 0;
    int rangeSeconds = 0;
};

struct NukeTarget {
    QString messageID;
    QString userID;
    QString loginName;
    QString displayName;
};

struct NukeMatches {
    QVector<NukeTarget> messageTargets;
    QVector<NukeTarget> userTargets;
    QSet<QString> highlightedMessageIDs;
    QSet<QString> seenUsers;

    int matchingMessages = 0;
    int skippedBroadcaster = 0;
    int skippedMods = 0;
    int skippedVips = 0;
    int skippedInvalid = 0;
};

struct NukeJob {
    QString channelKey;
    ParseResult plan;
    std::weak_ptr<Channel> channel;
    std::weak_ptr<TwitchChannel> twitchChannel;
    QString moderatorID;
    pajlada::Signals::ScopedConnection messageConnection;

    QVector<NukeTarget> queue;
    QSet<QString> processedMessages;
    QSet<QString> processedUsers;

    bool stopped = false;
    bool windowOpen = true;
    bool queueScheduled = false;
    bool fatalError = false;
    int extraDelayMs = 0;
    int inFlight = 0;

    int matchingMessages = 0;
    int queuedActions = 0;
    int completedActions = 0;
    int failedActions = 0;
    int skippedBroadcaster = 0;
    int skippedMods = 0;
    int skippedVips = 0;
    int skippedInvalid = 0;
};

struct SpamJob {
    QString channelKey;
    std::weak_ptr<TwitchChannel> channel;
    QVector<QString> messages;
    QSet<QString> normalizedMessages;
    QString senderLogin;
    QString senderUserID;
    QString label = QStringLiteral("Spam");
    QString commandName = QStringLiteral("/spam");
    pajlada::Signals::ScopedConnection messageConnection;
    int total = 0;
    int sent = 0;
    int echoed = 0;
    int intervalMs = DEFAULT_SPAM_INTERVAL_MS;
    int finishWaitMs = 0;
    bool useIrc = false;
    bool stopped = false;
    bool allMessagesQueued = false;
    bool finishScheduled = false;
    bool finished = false;
};

QHash<QString, QVector<std::shared_ptr<NukeJob>>> &activeNukes()
{
    static QHash<QString, QVector<std::shared_ptr<NukeJob>>> jobs;
    return jobs;
}

QHash<QString, std::shared_ptr<SpamJob>> &activeSpams()
{
    static QHash<QString, std::shared_ptr<SpamJob>> jobs;
    return jobs;
}

QString usage()
{
    return QStringLiteral(
        "Usage: /nuke <text> <timeout|ban|delete> <range>, for example /nuke "
        "bots 10m 30s. Use /nuke stop to cancel active nukes.");
}

QString spamUsage()
{
    return QStringLiteral("Usage: /spam <count> <message>, for example /spam "
                          "50 get in the predict. Use /spam stop to cancel.");
}

QString pyramidUsage()
{
    return QStringLiteral("Usage: /pyramid <height> <message>, for example "
                          "/pyramid 6 Kappa. Use /pyramid stop to cancel.");
}

QString actionName(const ParseResult &plan)
{
    switch (plan.action)
    {
        case NukeAction::Ban:
            return QStringLiteral("banned");
        case NukeAction::Delete:
            return QStringLiteral("deleted");
        case NukeAction::Timeout:
        default:
            return QStringLiteral("timed out");
    }
}

QString formatNukeRange(int seconds)
{
    if (seconds % 60 == 0)
    {
        return QStringLiteral("%1m").arg(seconds / 60);
    }

    return QStringLiteral("%1s").arg(seconds);
}

bool isInvisibleCodePoint(uint codePoint)
{
    return codePoint == 0x034F || codePoint == 0xFEFF ||
           (codePoint >= 0x200B && codePoint <= 0x200D) ||
           codePoint == 0xDB40 || (codePoint >= 0xDC00 && codePoint <= 0xDC7F);
}

QString normalizeExact(const QString &text)
{
    QString normalized;
    normalized.reserve(text.size());
    bool pendingSpace = false;

    for (const auto &ch : text.toCaseFolded())
    {
        const auto codePoint = ch.unicode();
        if (isInvisibleCodePoint(codePoint))
        {
            continue;
        }
        if (ch.isSpace())
        {
            pendingSpace = !normalized.isEmpty();
            continue;
        }
        if (pendingSpace)
        {
            normalized.append(u' ');
            pendingSpace = false;
        }
        normalized.append(ch);
    }

    return normalized.trimmed();
}

QString repeatedPyramidRow(const QString &message, int count)
{
    QStringList parts;
    parts.reserve(count);
    for (int i = 0; i < count; i++)
    {
        parts.append(message);
    }

    return parts.join(QLatin1Char(' '));
}

QVector<QString> repeatedMessages(const QString &message, int count)
{
    QVector<QString> messages;
    messages.reserve(count);
    for (int i = 0; i < count; i++)
    {
        messages.push_back(message);
    }
    return messages;
}

QVector<QString> buildPyramidMessages(const QString &message, int height)
{
    QVector<QString> messages;
    messages.reserve((height * 2) - 1);

    for (int level = 1; level <= height; level++)
    {
        messages.push_back(repeatedPyramidRow(message, level));
    }
    for (int level = height - 1; level >= 1; level--)
    {
        messages.push_back(repeatedPyramidRow(message, level));
    }

    return messages;
}

QString normalizeLoose(const QString &text)
{
    QString normalized;
    normalized.reserve(text.size());
    bool pendingSpace = false;
    QChar lastChar;

    for (const auto &ch : text.toCaseFolded())
    {
        const auto codePoint = ch.unicode();
        if (isInvisibleCodePoint(codePoint))
        {
            continue;
        }

        const bool keepChar = ch.isLetterOrNumber() || ch == u'_';
        if (!keepChar)
        {
            pendingSpace = !normalized.isEmpty();
            lastChar = QChar();
            continue;
        }

        if (pendingSpace)
        {
            normalized.append(u' ');
            pendingSpace = false;
            lastChar = QChar();
        }

        if (ch == lastChar)
        {
            continue;
        }

        normalized.append(ch);
        lastChar = ch;
    }

    return normalized.trimmed();
}

QStringList looseTokens(const QString &text)
{
    return normalizeLoose(text).split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

bool repeatedSuffixVariant(const QString &needle, const QString &candidate)
{
    if (needle.isEmpty() || candidate.isEmpty())
    {
        return false;
    }
    if (candidate == needle)
    {
        return true;
    }
    if (!candidate.startsWith(needle))
    {
        return false;
    }

    const auto repeated = needle.back();
    for (qsizetype i = needle.size(); i < candidate.size(); ++i)
    {
        if (candidate.at(i) != repeated)
        {
            return false;
        }
    }
    return candidate.size() <= needle.size() + 6;
}

bool tokenMatches(const QString &needle, const QString &candidate)
{
    if (needle == candidate || repeatedSuffixVariant(needle, candidate))
    {
        return true;
    }

    if (needle.size() <= 2)
    {
        return false;
    }

    if (candidate.startsWith(needle))
    {
        const auto maxExtraChars = needle.size() <= 3   ? 1
                                   : needle.size() <= 5 ? 2
                                   : needle.size() <= 7 ? 3
                                                        : 4;
        return candidate.size() <= needle.size() + maxExtraChars;
    }

    return false;
}

bool looseMessageMatches(const QString &target, const QString &message)
{
    const auto targetTokens = looseTokens(target);
    if (targetTokens.isEmpty())
    {
        return false;
    }

    const auto messageTokens = looseTokens(message);
    if (messageTokens.isEmpty())
    {
        return false;
    }

    if (targetTokens.size() == 1)
    {
        return std::any_of(messageTokens.begin(), messageTokens.end(),
                           [&](const QString &token) {
                               return tokenMatches(targetTokens.front(), token);
                           });
    }

    qsizetype searchFrom = 0;
    qsizetype lastMatch = -1;
    for (const auto &targetToken : targetTokens)
    {
        bool found = false;
        for (qsizetype i = searchFrom; i < messageTokens.size(); ++i)
        {
            if (lastMatch >= 0 && i - lastMatch > 4)
            {
                break;
            }
            if (tokenMatches(targetToken, messageTokens.at(i)))
            {
                lastMatch = i;
                searchFrom = i + 1;
                found = true;
                break;
            }
        }
        if (!found)
        {
            return false;
        }
    }

    return true;
}

bool messageMatches(const ParseResult &plan, const MessagePtr &message)
{
    if (message == nullptr)
    {
        return false;
    }

    if (plan.exact)
    {
        return normalizeExact(message->messageText) ==
               normalizeExact(plan.targetText);
    }

    return looseMessageMatches(plan.targetText, message->messageText);
}

bool hasBadge(const MessagePtr &message, const QString &badgeName)
{
    if (message == nullptr)
    {
        return false;
    }

    return std::any_of(
        message->twitchBadges.begin(), message->twitchBadges.end(),
        [&](const TwitchBadge &badge) {
            return badge.key_.compare(badgeName, Qt::CaseInsensitive) == 0;
        });
}

bool isModeratorMessage(const MessagePtr &message)
{
    return hasBadge(message, QStringLiteral("moderator")) ||
           hasBadge(message, QStringLiteral("lead_moderator"));
}

bool isBroadcasterMessage(const MessagePtr &message, const ChannelPtr &channel)
{
    if (hasBadge(message, QStringLiteral("broadcaster")))
    {
        return true;
    }
    return message != nullptr && channel != nullptr &&
           message->loginName.compare(channel->getName(),
                                      Qt::CaseInsensitive) == 0;
}

std::optional<NukeTarget> makeTarget(const MessagePtr &message)
{
    if (message == nullptr || message->id.isEmpty() ||
        message->userID.isEmpty())
    {
        return std::nullopt;
    }

    return NukeTarget{
        .messageID = message->id,
        .userID = message->userID,
        .loginName = message->loginName,
        .displayName = message->displayName.isEmpty() ? message->loginName
                                                      : message->displayName,
    };
}

bool inRange(const MessagePtr &message, const QDateTime &cutoff)
{
    if (message == nullptr || !message->serverReceivedTime.isValid())
    {
        return false;
    }

    return message->serverReceivedTime >= cutoff;
}

bool isFatalForwardedModerationError(const QString &message)
{
    const auto lower = message.toLower();
    return lower.contains("auth") || lower.contains("scope") ||
           lower.contains("permission") || lower.contains("authorized") ||
           lower.contains("token");
}

bool parseActionText(const QString &actionText, ParseResult &result)
{
    if (actionText == QStringLiteral("ban"))
    {
        result.action = NukeAction::Ban;
        result.timeoutSeconds = 0;
        return true;
    }
    if (actionText == QStringLiteral("delete"))
    {
        result.action = NukeAction::Delete;
        result.timeoutSeconds = 0;
        return true;
    }

    const auto duration = parseDurationToSeconds(actionText);
    if (duration <= 0)
    {
        return false;
    }

    result.action = NukeAction::Timeout;
    result.timeoutSeconds = std::min<int64_t>(duration, MAX_TIMEOUT_SECONDS);
    return true;
}

void addMatch(NukeMatches &matches, const ParseResult &plan,
              const ChannelPtr &channel, const MessagePtr &message)
{
    if (!messageMatches(plan, message))
    {
        return;
    }

    matches.matchingMessages++;

    if (isBroadcasterMessage(message, channel))
    {
        matches.skippedBroadcaster++;
        return;
    }

    if (isModeratorMessage(message))
    {
        matches.skippedMods++;
        return;
    }

    if (getSettings()->nukeSkipVips && hasBadge(message, QStringLiteral("vip")))
    {
        matches.skippedVips++;
        return;
    }

    auto target = makeTarget(message);
    if (!target)
    {
        matches.skippedInvalid++;
        return;
    }

    matches.highlightedMessageIDs.insert(target->messageID);
    if (plan.action == NukeAction::Delete)
    {
        matches.messageTargets.push_back(*target);
        return;
    }

    if (matches.seenUsers.contains(target->userID))
    {
        return;
    }

    matches.seenUsers.insert(target->userID);
    matches.userTargets.push_back(*target);
}

NukeMatches collectPastMatches(const ParseResult &plan,
                               const ChannelPtr &channel)
{
    NukeMatches matches;
    if (channel == nullptr)
    {
        return matches;
    }

    const auto cutoff =
        QDateTime::currentDateTimeUtc().addSecs(-plan.rangeSeconds);
    const auto snapshot = channel->getMessageSnapshot();

    for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it)
    {
        const auto &message = *it;
        if (message == nullptr)
        {
            continue;
        }
        if (message->flags.hasAny({MessageFlag::System, MessageFlag::Timeout,
                                   MessageFlag::Whisper}))
        {
            continue;
        }
        if (message->serverReceivedTime.isValid() &&
            message->serverReceivedTime < cutoff)
        {
            break;
        }
        if (!inRange(message, cutoff))
        {
            continue;
        }

        addMatch(matches, plan, channel, message);
    }

    return matches;
}

void setPreviewTarget(ParseResult &result, const QStringList &targetWords,
                      const QString &rawRest)
{
    result.targetText = targetWords.join(QLatin1Char(' ')).trimmed();
    result.exact = rawRest.startsWith(QLatin1Char('"'));
}

ParseResult parseNukeInput(const QString &input, bool allowPartialPreview)
{
    ParseResult result;
    const auto trimmed = input.trimmed();
    if (!trimmed.startsWith(QStringLiteral("/nuke"), Qt::CaseInsensitive))
    {
        return result;
    }

    const auto firstSpace = trimmed.indexOf(u' ');
    const auto commandName =
        firstSpace < 0 ? trimmed : trimmed.left(firstSpace).trimmed();
    if (commandName.compare(QStringLiteral("/nuke"), Qt::CaseInsensitive) != 0)
    {
        return result;
    }

    result.isNuke = true;

    const auto words = QProcess::splitCommand(trimmed);
    if (words.size() == 2 &&
        words.at(1).compare(QStringLiteral("stop"), Qt::CaseInsensitive) == 0)
    {
        result.isStop = true;
        result.complete = true;
        return result;
    }

    const auto rawRest =
        firstSpace < 0 ? QString() : trimmed.mid(firstSpace).trimmed();

    auto completeWithDefaultRange = [&](const QStringList &targetWords,
                                        const QString &actionText) {
        ParseResult preview = result;
        if (!parseActionText(actionText.toLower(), preview))
        {
            return std::optional<ParseResult>{};
        }

        setPreviewTarget(preview, targetWords, rawRest);
        if (preview.targetText.isEmpty())
        {
            return std::optional<ParseResult>{};
        }

        preview.rangeSeconds = MAX_NUKE_RANGE_SECONDS;
        preview.complete = true;
        return std::optional<ParseResult>{preview};
    };

    if (allowPartialPreview && words.size() == 3)
    {
        const auto preview = completeWithDefaultRange(
            words.mid(1, words.size() - 2), words.back());
        if (preview)
        {
            return *preview;
        }
    }

    if (words.size() < 4)
    {
        if (!allowPartialPreview)
        {
            return result;
        }

        setPreviewTarget(result, words.mid(1), rawRest);
        if (result.targetText.isEmpty())
        {
            return result;
        }
        result.rangeSeconds = MAX_NUKE_RANGE_SECONDS;
        result.complete = true;
        return result;
    }

    const auto rangeText = words.at(words.size() - 1).toLower();
    const auto actionText = words.at(words.size() - 2).toLower();
    setPreviewTarget(result, words.mid(1, words.size() - 3), rawRest);
    if (result.targetText.isEmpty())
    {
        result.error = usage();
        return result;
    }

    const bool hasCompleteAction = parseActionText(actionText, result);
    const auto range = parseDurationToSeconds(rangeText);

    if (allowPartialPreview &&
        (!hasCompleteAction || range <= 0 || range > MAX_NUKE_RANGE_SECONDS))
    {
        const auto preview = completeWithDefaultRange(
            words.mid(1, words.size() - 2), words.back());
        if (preview)
        {
            return *preview;
        }
    }

    if (!hasCompleteAction)
    {
        result.error = QStringLiteral(
            "Invalid /nuke action. Use a timeout duration, ban, or delete.");
        return result;
    }

    if (range <= 0)
    {
        result.error = QStringLiteral("Invalid /nuke range.");
        return result;
    }
    if (range > MAX_NUKE_RANGE_SECONDS)
    {
        result.error = QStringLiteral("/nuke range is limited to 10 minutes.");
        return result;
    }
    result.rangeSeconds = static_cast<int>(range);
    result.complete = true;
    return result;
}

QString previewStatus(const ParseResult &plan, const NukeMatches &matches)
{
    const auto rangeText = formatNukeRange(plan.rangeSeconds);
    if (matches.highlightedMessageIDs.isEmpty())
    {
        return QStringLiteral("Nuke preview: no matching messages in last %1")
            .arg(rangeText);
    }

    QString targetCount;
    if (plan.action == NukeAction::Delete)
    {
        targetCount =
            QStringLiteral("%1 message%2")
                .arg(matches.highlightedMessageIDs.size())
                .arg(matches.highlightedMessageIDs.size() == 1 ? "" : "s");
    }
    else
    {
        targetCount =
            QStringLiteral("%1 message%2 from %3 user%4")
                .arg(matches.highlightedMessageIDs.size())
                .arg(matches.highlightedMessageIDs.size() == 1 ? "" : "s")
                .arg(matches.userTargets.size())
                .arg(matches.userTargets.size() == 1 ? "" : "s");
    }

    QStringList skipped;
    if (matches.skippedBroadcaster > 0)
    {
        skipped.append(
            QStringLiteral("%1 broadcaster").arg(matches.skippedBroadcaster));
    }
    if (matches.skippedMods > 0)
    {
        skipped.append(QStringLiteral("%1 mod%2")
                           .arg(matches.skippedMods)
                           .arg(matches.skippedMods == 1 ? "" : "s"));
    }
    if (matches.skippedVips > 0)
    {
        skipped.append(QStringLiteral("%1 VIP%2")
                           .arg(matches.skippedVips)
                           .arg(matches.skippedVips == 1 ? "" : "s"));
    }

    if (skipped.isEmpty())
    {
        return QStringLiteral("Nuke preview: %1 targeted in last %2")
            .arg(targetCount, rangeText);
    }

    return QStringLiteral("Nuke preview: %1 targeted in last %2, %3 skipped")
        .arg(targetCount, rangeText, skipped.join(QStringLiteral(", ")));
}

QString channelKey(TwitchChannel *channel)
{
    if (channel == nullptr)
    {
        return {};
    }
    const auto roomID = channel->roomId();
    return roomID.isEmpty() ? channel->getName().toLower() : roomID;
}

void removeNukeJob(const std::shared_ptr<NukeJob> &job)
{
    auto &jobsByChannel = activeNukes();
    auto it = jobsByChannel.find(job->channelKey);
    if (it == jobsByChannel.end())
    {
        return;
    }

    auto &jobs = it.value();
    for (int i = 0; i < jobs.size(); ++i)
    {
        if (jobs.at(i).get() == job.get())
        {
            jobs.removeAt(i);
            break;
        }
    }
    if (jobs.isEmpty())
    {
        jobsByChannel.erase(it);
    }
}

void maybeFinishNukeJob(const std::shared_ptr<NukeJob> &job)
{
    if (job->windowOpen || job->inFlight > 0 || !job->queue.isEmpty())
    {
        return;
    }
    if (job->stopped)
    {
        removeNukeJob(job);
        return;
    }

    auto channel = job->channel.lock();
    if (channel != nullptr && getSettings()->nukeShowSummary)
    {
        QStringList parts;
        if (job->plan.action == NukeAction::Delete)
        {
            parts.append(QStringLiteral("%1 message%2 deleted")
                             .arg(job->completedActions)
                             .arg(job->completedActions == 1 ? "" : "s"));
        }
        else
        {
            parts.append(QStringLiteral("%1 user%2 %3")
                             .arg(job->completedActions)
                             .arg(job->completedActions == 1 ? "" : "s")
                             .arg(actionName(job->plan)));
        }
        if (job->failedActions > 0)
        {
            parts.append(QStringLiteral("%1 failed").arg(job->failedActions));
        }
        if (job->skippedBroadcaster > 0)
        {
            parts.append(QStringLiteral("%1 broadcaster skipped")
                             .arg(job->skippedBroadcaster));
        }
        if (job->skippedMods > 0)
        {
            parts.append(QStringLiteral("%1 mod%2 skipped")
                             .arg(job->skippedMods)
                             .arg(job->skippedMods == 1 ? "" : "s"));
        }
        if (job->skippedVips > 0)
        {
            parts.append(QStringLiteral("%1 VIP%2 skipped")
                             .arg(job->skippedVips)
                             .arg(job->skippedVips == 1 ? "" : "s"));
        }

        channel->addSystemMessage(QStringLiteral("Nuke finished: %1")
                                      .arg(parts.join(QStringLiteral(", "))));
    }

    removeNukeJob(job);
}

void scheduleNukePump(const std::shared_ptr<NukeJob> &job);

void onNukeActionFinished(const std::shared_ptr<NukeJob> &job, bool success,
                          bool rateLimited, bool fatal)
{
    job->inFlight = std::max(0, job->inFlight - 1);
    if (success)
    {
        job->completedActions++;
    }
    else
    {
        job->failedActions++;
    }

    if (rateLimited)
    {
        job->extraDelayMs =
            std::max(job->extraDelayMs, NUKE_RATE_LIMIT_BACKOFF_MS);
    }
    if (fatal)
    {
        if (!job->fatalError)
        {
            if (auto channel = job->channel.lock())
            {
                channel->addSystemMessage(
                    "Nuke stopped because Twitch rejected the moderation "
                    "request. Re-authenticate or check your permissions.");
            }
        }
        job->fatalError = true;
        job->stopped = true;
        job->queue.clear();
    }

    scheduleNukePump(job);
    maybeFinishNukeJob(job);
}

void performNukeAction(const std::shared_ptr<NukeJob> &job,
                       const NukeTarget &target)
{
    auto channel = job->channel.lock();
    auto twitchChannel = job->twitchChannel.lock();
    if (channel == nullptr || twitchChannel == nullptr)
    {
        onNukeActionFinished(job, false, false, true);
        return;
    }

    job->inFlight++;
    switch (job->plan.action)
    {
        case NukeAction::Delete: {
            getHelix()->deleteChatMessages(
                twitchChannel->roomId(), job->moderatorID, target.messageID,
                [job] {
                    onNukeActionFinished(job, true, false, false);
                },
                [job](HelixDeleteChatMessagesError error, const auto &message) {
                    const bool fatal =
                        error ==
                            HelixDeleteChatMessagesError::UserMissingScope ||
                        error ==
                            HelixDeleteChatMessagesError::UserNotAuthorized ||
                        error == HelixDeleteChatMessagesError::
                                     UserNotAuthenticated ||
                        (error == HelixDeleteChatMessagesError::Forwarded &&
                         isFatalForwardedModerationError(message));
                    onNukeActionFinished(job, false, false, fatal);
                });
            break;
        }
        case NukeAction::Ban:
        case NukeAction::Timeout: {
            const std::optional<int> duration =
                job->plan.action == NukeAction::Timeout
                    ? std::optional<int>(job->plan.timeoutSeconds)
                    : std::nullopt;
            getHelix()->banUser(
                twitchChannel->roomId(), job->moderatorID, target.userID,
                duration,
                getSettings()->nukeModerationMessage.getValue().trimmed(),
                [job] {
                    onNukeActionFinished(job, true, false, false);
                },
                [job](HelixBanUserError error, const auto &message) {
                    const bool rateLimited =
                        error == HelixBanUserError::Ratelimited;
                    const bool fatal =
                        error == HelixBanUserError::UserMissingScope ||
                        error == HelixBanUserError::UserNotAuthorized ||
                        (error == HelixBanUserError::Forwarded &&
                         isFatalForwardedModerationError(message));
                    onNukeActionFinished(job, false, rateLimited, fatal);
                });
            break;
        }
    }
}

void pumpNukeQueue(const std::shared_ptr<NukeJob> &job)
{
    job->queueScheduled = false;
    if (job->stopped || job->fatalError || job->queue.isEmpty())
    {
        maybeFinishNukeJob(job);
        return;
    }

    const auto target = job->queue.takeFirst();
    performNukeAction(job, target);

    if (!job->queue.isEmpty())
    {
        scheduleNukePump(job);
    }
}

void scheduleNukePump(const std::shared_ptr<NukeJob> &job)
{
    if (job->queueScheduled || job->stopped || job->fatalError ||
        job->queue.isEmpty())
    {
        maybeFinishNukeJob(job);
        return;
    }

    job->queueScheduled = true;
    const auto baseDelay = job->plan.action == NukeAction::Delete
                               ? NUKE_DELETE_INTERVAL_MS
                               : NUKE_MODERATION_INTERVAL_MS;
    const auto delay = baseDelay + job->extraDelayMs;
    job->extraDelayMs = 0;
    QTimer::singleShot(delay, [job] {
        pumpNukeQueue(job);
    });
}

void enqueueTarget(const std::shared_ptr<NukeJob> &job,
                   const NukeTarget &target)
{
    if (job->plan.action == NukeAction::Delete)
    {
        if (job->processedMessages.contains(target.messageID))
        {
            return;
        }
        job->processedMessages.insert(target.messageID);
    }
    else
    {
        if (job->processedUsers.contains(target.userID))
        {
            return;
        }
        job->processedUsers.insert(target.userID);
    }

    job->queue.push_back(target);
    job->queuedActions++;
    scheduleNukePump(job);
}

void enqueueMatches(const std::shared_ptr<NukeJob> &job,
                    const NukeMatches &matches)
{
    job->matchingMessages += matches.matchingMessages;
    job->skippedBroadcaster += matches.skippedBroadcaster;
    job->skippedMods += matches.skippedMods;
    job->skippedVips += matches.skippedVips;
    job->skippedInvalid += matches.skippedInvalid;

    const auto &targets = job->plan.action == NukeAction::Delete
                              ? matches.messageTargets
                              : matches.userTargets;
    for (const auto &target : targets)
    {
        enqueueTarget(job, target);
    }
}

void processFutureMessage(const std::shared_ptr<NukeJob> &job,
                          MessagePtr &message)
{
    if (job->stopped || job->fatalError || !job->windowOpen)
    {
        return;
    }

    auto channel = job->channel.lock();
    if (channel == nullptr || message == nullptr ||
        message->flags.hasAny(
            {MessageFlag::System, MessageFlag::Timeout, MessageFlag::Whisper}))
    {
        return;
    }

    NukeMatches matches;
    addMatch(matches, job->plan, channel, message);
    enqueueMatches(job, matches);
}

void stopNukesForChannel(TwitchChannel *channel)
{
    const auto key = channelKey(channel);
    auto &jobsByChannel = activeNukes();
    auto jobs = jobsByChannel.take(key);
    for (const auto &job : jobs)
    {
        job->stopped = true;
        job->windowOpen = false;
        job->queue.clear();
        job->messageConnection = pajlada::Signals::ScopedConnection();
    }

    channel->addSystemMessage(
        jobs.isEmpty() ? QStringLiteral("No active nukes in this channel.")
                       : QStringLiteral("Stopped %1 active nuke%2.")
                             .arg(jobs.size())
                             .arg(jobs.size() == 1 ? "" : "s"));
}

void startNukeJob(const CommandContext &ctx, const ParseResult &plan)
{
    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser->isAnon())
    {
        ctx.channel->addSystemMessage("You must be logged in to use /nuke.");
        return;
    }

    if (!ctx.twitchChannel->hasModRights())
    {
        ctx.channel->addSystemMessage(
            "You need moderation rights in this channel to use /nuke.");
        return;
    }

    const auto key = channelKey(ctx.twitchChannel);
    auto job = std::make_shared<NukeJob>();
    job->channelKey = key;
    job->plan = plan;
    job->channel = ctx.channel;
    job->twitchChannel = std::dynamic_pointer_cast<TwitchChannel>(ctx.channel);
    job->moderatorID = currentUser->getUserId();

    auto &jobs = activeNukes()[key];
    jobs.push_back(job);

    auto pastMatches = collectPastMatches(plan, ctx.channel);
    enqueueMatches(job, pastMatches);

    job->messageConnection = ctx.channel->messageAppended.connect(
        [job](MessagePtr &message, auto) mutable {
            processFutureMessage(job, message);
        });

    QTimer::singleShot(plan.rangeSeconds * 1000, [job] {
        job->windowOpen = false;
        job->messageConnection = pajlada::Signals::ScopedConnection();
        maybeFinishNukeJob(job);
    });

    const auto targetCount = plan.action == NukeAction::Delete
                                 ? pastMatches.messageTargets.size()
                                 : pastMatches.userTargets.size();
    ctx.channel->addSystemMessage(
        QStringLiteral("Nuke armed for %1s: %2 past target%3 queued.")
            .arg(plan.rangeSeconds)
            .arg(targetCount)
            .arg(targetCount == 1 ? "" : "s"));

    maybeFinishNukeJob(job);
}

void removeSpamJob(const std::shared_ptr<SpamJob> &job)
{
    job->messageConnection = pajlada::Signals::ScopedConnection();
    auto &jobs = activeSpams();
    auto it = jobs.find(job->channelKey);
    if (it != jobs.end() && it.value().get() == job.get())
    {
        jobs.erase(it);
    }
}

void stopSpamJobForSendFailure(const std::shared_ptr<SpamJob> &job,
                               const QString &error)
{
    if (job->stopped || job->finished)
    {
        return;
    }

    job->stopped = true;
    if (auto channel = job->channel.lock())
    {
        const bool rateLimited =
            error.contains("sending messages too quickly", Qt::CaseInsensitive);
        channel->addSystemMessage(
            error.isEmpty()
                ? QStringLiteral(
                      "%1 stopped because a message could not be sent.")
                      .arg(job->label)
            : rateLimited
                ? QStringLiteral("%1 stopped: Twitch rate-limited this account "
                                 "in this channel. "
                                 "Fast bursts usually only work reliably when "
                                 "the account is mod, VIP, or broadcaster.")
                      .arg(job->label)
                : QStringLiteral("%1 stopped: %2").arg(job->label, error));
    }
    removeSpamJob(job);
}

void finishSpamJob(const std::shared_ptr<SpamJob> &job)
{
    if (job->stopped || job->finished)
    {
        return;
    }

    job->finished = true;
    if (auto channel = job->channel.lock())
    {
        if (getSettings()->showSpamPyramidStatusMessages)
        {
            channel->addSystemMessage(
                QStringLiteral("%1 finished: sent %2 message%3.")
                    .arg(job->label)
                    .arg(job->sent)
                    .arg(job->sent == 1 ? "" : "s"));
        }
    }
    removeSpamJob(job);
}

void maybeFinishSpamJob(const std::shared_ptr<SpamJob> &job)
{
    if (!job->allMessagesQueued || job->stopped || job->finished)
    {
        return;
    }

    if (job->echoed >= job->sent)
    {
        finishSpamJob(job);
        return;
    }

    if (!job->finishScheduled)
    {
        job->finishScheduled = true;
        QTimer::singleShot(SPAM_FINISH_CHECK_MS, [job] {
            job->finishScheduled = false;
            job->finishWaitMs += SPAM_FINISH_CHECK_MS;
            if (job->echoed >= job->sent ||
                job->finishWaitMs >= SPAM_FINISH_MAX_WAIT_MS)
            {
                finishSpamJob(job);
                return;
            }
            maybeFinishSpamJob(job);
        });
    }
}

void runSpamStep(const std::shared_ptr<SpamJob> &job)
{
    auto channel = job->channel.lock();
    if (job->stopped || channel == nullptr)
    {
        removeSpamJob(job);
        return;
    }

    if (job->sent >= job->total)
    {
        job->allMessagesQueued = true;
        maybeFinishSpamJob(job);
        return;
    }

    const auto nextNonce = job->sent + 1;
    const auto &message = job->messages.at(job->sent);
    const auto queued =
        job->useIrc
            ? channel->sendMessageViaIrc(message, nextNonce)
            : channel->sendSpamMessageViaHelix(
                  message, nextNonce, [job](bool sent, const QString &error) {
                      if (sent || job->stopped || job->finished)
                      {
                          return;
                      }

                      stopSpamJobForSendFailure(job, error);
                  });

    if (!queued)
    {
        stopSpamJobForSendFailure(job, {});
        return;
    }
    job->sent++;

    QTimer::singleShot(job->intervalMs, [job] {
        runSpamStep(job);
    });
}

void stopSpamForChannel(TwitchChannel *channel, const QString &requestedLabel)
{
    const auto key = channelKey(channel);
    auto &jobs = activeSpams();
    auto it = jobs.find(key);
    if (it == jobs.end())
    {
        channel->addSystemMessage(
            QStringLiteral("No active %1 in this channel.")
                .arg(requestedLabel));
        return;
    }

    const auto sent = it.value()->sent;
    const auto label = it.value()->label.toLower();
    it.value()->stopped = true;
    it.value()->messageConnection = pajlada::Signals::ScopedConnection();
    jobs.erase(it);
    channel->addSystemMessage(QStringLiteral("Stopped %1 after %2 message%3.")
                                  .arg(label)
                                  .arg(sent)
                                  .arg(sent == 1 ? "" : "s"));
}

void startChatMessageJob(const CommandContext &ctx, QVector<QString> messages,
                         const QString &label, const QString &commandName,
                         const QString &startVerb, bool useIrc,
                         int minIntervalMs = MIN_SPAM_INTERVAL_MS)
{
    if (messages.isEmpty())
    {
        return;
    }

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser->isAnon())
    {
        ctx.channel->addSystemMessage(
            QStringLiteral("You must be logged in to use %1.")
                .arg(commandName));
        return;
    }

    const auto key = channelKey(ctx.twitchChannel);
    if (activeSpams().contains(key))
    {
        const auto active = activeSpams().value(key);
        ctx.channel->addSystemMessage(
            QStringLiteral(
                "%1 is already running in this channel. Use %2 stop first.")
                .arg(active->label, active->commandName));
        return;
    }

    auto job = std::make_shared<SpamJob>();
    job->channelKey = key;
    job->channel = std::dynamic_pointer_cast<TwitchChannel>(ctx.channel);
    job->messages = std::move(messages);
    job->senderLogin = currentUser->getUserName().toLower();
    job->senderUserID = currentUser->getUserId();
    job->label = label;
    job->commandName = commandName;
    job->total = job->messages.size();
    job->intervalMs =
        std::clamp(getSettings()->spamCommandIntervalMs.getValue(),
                   minIntervalMs, MAX_SPAM_INTERVAL_MS);
    job->useIrc = useIrc;
    for (const auto &message : job->messages)
    {
        job->normalizedMessages.insert(normalizeExact(message));
    }
    activeSpams()[key] = job;

    job->messageConnection = ctx.channel->messageAppended.connect(
        [job](MessagePtr &message, auto) mutable {
            if (message == nullptr || job->stopped || job->finished)
            {
                return;
            }

            const bool sameUser =
                (!job->senderUserID.isEmpty() &&
                 message->userID == job->senderUserID) ||
                (!job->senderLogin.isEmpty() &&
                 message->loginName.compare(job->senderLogin,
                                            Qt::CaseInsensitive) == 0);
            if (!sameUser || !job->normalizedMessages.contains(
                                 normalizeExact(message->messageText)))
            {
                return;
            }

            job->echoed++;
            maybeFinishSpamJob(job);
        });

    if (!getSettings()->showSpamPyramidStatusMessages)
    {
        runSpamStep(job);
        return;
    }

    if (commandName == QStringLiteral("/pyramid"))
    {
        ctx.channel->addSystemMessage(
            "Building pyramid... Use /pyramid stop to cancel.");
    }
    else
    {
        ctx.channel->addSystemMessage(
            QStringLiteral("%1 %2 message%3 through %4 every %5 ms. Use %6 "
                           "stop to cancel.")
                .arg(startVerb)
                .arg(job->total)
                .arg(job->total == 1 ? "" : "s")
                .arg(job->useIrc ? QStringLiteral("IRC")
                                 : QStringLiteral("Helix"))
                .arg(job->intervalMs)
                .arg(commandName));
    }

    runSpamStep(job);
}

}  // namespace

namespace chatterino::commands {

NukePreview buildNukePreview(const QString &input, const ChannelPtr &channel)
{
    NukePreview preview;
    if (!getSettings()->nukePreviewEnabled)
    {
        return preview;
    }
    if (std::dynamic_pointer_cast<TwitchChannel>(channel) == nullptr)
    {
        return preview;
    }

    auto parsed = parseNukeInput(input, true);
    if (!parsed.isNuke || parsed.isStop)
    {
        return preview;
    }

    if (!parsed.error.isEmpty())
    {
        preview.active = true;
        preview.statusText = parsed.error;
        return preview;
    }

    if (!parsed.complete)
    {
        return preview;
    }

    auto matches = collectPastMatches(parsed, channel);
    preview.active = true;
    preview.messageIDs = matches.highlightedMessageIDs;
    preview.statusText = previewStatus(parsed, matches);
    return preview;
}

QString sendNuke(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }
    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage("/nuke only works in Twitch channels.");
        return "";
    }

    const auto commandText =
        ctx.rawText.isEmpty() ? ctx.words.join(QLatin1Char(' ')) : ctx.rawText;
    const auto parsed = parseNukeInput(commandText, false);
    if (parsed.isStop)
    {
        stopNukesForChannel(ctx.twitchChannel);
        return "";
    }

    if (!parsed.complete || !parsed.error.isEmpty())
    {
        ctx.channel->addSystemMessage(parsed.error.isEmpty() ? usage()
                                                             : parsed.error);
        return "";
    }

    startNukeJob(ctx, parsed);
    return "";
}

QString sendSpam(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }
    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage("/spam only works in Twitch channels.");
        return "";
    }

    if (ctx.words.size() >= 2 &&
        ctx.words.at(1).compare(QStringLiteral("stop"), Qt::CaseInsensitive) ==
            0)
    {
        stopSpamForChannel(ctx.twitchChannel, QStringLiteral("spam"));
        return "";
    }

    if (ctx.words.size() < 3)
    {
        ctx.channel->addSystemMessage(spamUsage());
        return "";
    }

    bool ok = false;
    const auto count = ctx.words.at(1).toInt(&ok);
    if (!ok || count <= 0)
    {
        ctx.channel->addSystemMessage(spamUsage());
        return "";
    }

    const auto cappedCount = std::min(count, MAX_SPAM_COUNT);
    const auto message = ctx.words.mid(2).join(QLatin1Char(' ')).trimmed();
    if (message.isEmpty())
    {
        ctx.channel->addSystemMessage(spamUsage());
        return "";
    }
    if (message.startsWith(QLatin1Char('/')) ||
        message.startsWith(QLatin1Char('.')))
    {
        ctx.channel->addSystemMessage(
            "/spam sends chat messages, not chat commands.");
        return "";
    }

    startChatMessageJob(ctx, repeatedMessages(message, cappedCount),
                        QStringLiteral("Spam"), QStringLiteral("/spam"),
                        QStringLiteral("Spamming"),
                        getSettings()->spamCommandUseIrc);
    return "";
}

QString sendPyramid(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }
    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "/pyramid only works in Twitch channels.");
        return "";
    }

    if (ctx.words.size() >= 2 &&
        ctx.words.at(1).compare(QStringLiteral("stop"), Qt::CaseInsensitive) ==
            0)
    {
        stopSpamForChannel(ctx.twitchChannel, QStringLiteral("pyramid"));
        return "";
    }

    if (ctx.words.size() < 3)
    {
        ctx.channel->addSystemMessage(pyramidUsage());
        return "";
    }

    bool ok = false;
    const auto height = ctx.words.at(1).toInt(&ok);
    if (!ok || height <= 0)
    {
        ctx.channel->addSystemMessage(pyramidUsage());
        return "";
    }

    const auto cappedHeight = std::min(height, MAX_PYRAMID_HEIGHT);
    const auto message = ctx.words.mid(2).join(QLatin1Char(' ')).trimmed();
    if (message.isEmpty())
    {
        ctx.channel->addSystemMessage(pyramidUsage());
        return "";
    }
    if (message.startsWith(QLatin1Char('/')) ||
        message.startsWith(QLatin1Char('.')))
    {
        ctx.channel->addSystemMessage(
            "/pyramid sends chat messages, not chat commands.");
        return "";
    }

    if (repeatedPyramidRow(message, cappedHeight).size() >
        MAX_PYRAMID_ROW_MESSAGE_LENGTH)
    {
        ctx.channel->addSystemMessage(
            "The top of that pyramid is too long. Use a shorter message or a "
            "lower height.");
        return "";
    }

    startChatMessageJob(ctx, buildPyramidMessages(message, cappedHeight),
                        QStringLiteral("Pyramid"), QStringLiteral("/pyramid"),
                        QStringLiteral("Building pyramid with"),
                        getSettings()->spamCommandUseIrc,
                        ctx.twitchChannel->hasHighRateLimit()
                            ? MIN_SPAM_INTERVAL_MS
                            : NORMAL_CHATTER_PYRAMID_INTERVAL_MS);
    return "";
}

}  // namespace chatterino::commands

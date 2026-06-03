// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/repetitions/RepeatedMessageDetector.hpp"

#include "singletons/Settings.hpp"

#include <QDateTime>

#include <algorithm>

namespace chatterino {

namespace {

constexpr qint64 CACHE_TTL_MS = 5 * 60 * 1000;
constexpr qsizetype MAX_SEEN_MESSAGE_IDS_PER_USER = 16;
constexpr qsizetype MAX_USERS_PER_CHANNEL = 2048;
constexpr qsizetype MAX_NORMALIZED_CHARS = 512;
constexpr int MAX_REPEAT_COUNT = 999;
constexpr int MAX_ACTIVE_STREAK_MISSES = 3;
constexpr int CLEANUP_INTERVAL_CHECKS = 128;

char32_t codePointAt(const QString &text, qsizetype index, qsizetype &next)
{
    next = index + 1;
    const auto first = text.at(index).unicode();
    if (QChar::isHighSurrogate(first) && index + 1 < text.size())
    {
        const auto second = text.at(index + 1).unicode();
        if (QChar::isLowSurrogate(second))
        {
            ++next;
            return QChar::surrogateToUcs4(first, second);
        }
    }

    return first;
}

bool isDuplicateBypassCodePoint(char32_t codePoint)
{
    return codePoint == 0x034F ||
           (codePoint >= 0xE0000 && codePoint <= 0xE007F);
}

}  // namespace

std::optional<int> RepeatedMessageDetector::check(
    const RepeatedMessageCheck &check)
{
    auto *settings = getSettings();
    if (!settings->enableRepeatedMessageDetector)
    {
        this->clear();
        return std::nullopt;
    }

    if (check.historical || check.channelID.isEmpty() ||
        check.userID.isEmpty() || check.messageID.isEmpty() ||
        check.message.isEmpty())
    {
        return std::nullopt;
    }

    if (settings->repeatedMessagesOnlyModChannels && !check.channelCanModerate)
    {
        return std::nullopt;
    }

    if (check.senderIsModerator || check.senderIsBroadcaster)
    {
        return std::nullopt;
    }

    if (settings->repeatedMessagesIgnoreVips && check.senderIsVip)
    {
        return std::nullopt;
    }

    auto normalized = normalizeMessage(check.message);
    if (normalized.isEmpty())
    {
        return std::nullopt;
    }

    const auto now = QDateTime::currentMSecsSinceEpoch();

    if (++this->checksSinceCleanup_ >= CLEANUP_INTERVAL_CHECKS)
    {
        this->checksSinceCleanup_ = 0;
        for (auto it = this->channels_.begin(); it != this->channels_.end();)
        {
            this->cleanupChannel(it.value(), now);
            if (it.value().isEmpty())
            {
                it = this->channels_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    auto &channel = this->channels_[check.channelID];
    auto &user = channel[check.userID];
    user.lastSeenAt = now;
    this->cleanupUser(user, now);

    if (hasSeenMessageID(user, check.messageID))
    {
        return std::nullopt;
    }
    rememberMessageID(user, check.messageID, now);

    const int sensitivityPercent =
        sensitivityToPercent(settings->repeatedMessagesSensitivity);
    const int visibleThreshold =
        std::max(2, settings->repeatedMessagesRepetitionThreshold.getValue());

    if (user.active && messagesMatch(normalized, user.active->normalizedMessage,
                                     sensitivityPercent))
    {
        user.active->repeatCount =
            std::min(user.active->repeatCount + 1, MAX_REPEAT_COUNT);
        user.active->messageID = check.messageID;
        user.active->missesSinceMatch = 0;
        user.active->expiresAt = now + CACHE_TTL_MS;
        user.candidate = user.active;
        this->enforceChannelLimit(channel);
        if (user.active->repeatCount >= visibleThreshold)
        {
            return user.active->repeatCount;
        }
        return std::nullopt;
    }

    if (user.active)
    {
        user.active->missesSinceMatch++;
        if (user.active->missesSinceMatch >= MAX_ACTIVE_STREAK_MISSES)
        {
            user.active.reset();
        }
    }

    if (user.candidate &&
        messagesMatch(normalized, user.candidate->normalizedMessage,
                      sensitivityPercent))
    {
        user.candidate->repeatCount =
            std::min(user.candidate->repeatCount + 1, MAX_REPEAT_COUNT);
        user.candidate->messageID = check.messageID;
        user.candidate->expiresAt = now + CACHE_TTL_MS;
    }
    else
    {
        user.candidate = makeEntry(normalized, check.messageID, now);
    }

    if (user.candidate->repeatCount >= visibleThreshold)
    {
        user.candidate->missesSinceMatch = 0;
        user.active = user.candidate;
        this->enforceChannelLimit(channel);
        return user.active->repeatCount;
    }

    this->enforceChannelLimit(channel);
    return std::nullopt;
}

void RepeatedMessageDetector::clear()
{
    this->channels_.clear();
    this->checksSinceCleanup_ = 0;
}

QString RepeatedMessageDetector::normalizeMessage(const QString &message)
{
    QString normalized;
    normalized.reserve(std::min(message.size(), MAX_NORMALIZED_CHARS));
    bool pendingSpace = false;

    const auto folded = message.toCaseFolded();
    for (qsizetype i = 0; i < folded.size();)
    {
        qsizetype next = i + 1;
        const auto codePoint = codePointAt(folded, i, next);
        i = next;

        if (isDuplicateBypassCodePoint(codePoint))
        {
            continue;
        }
        if (codePoint <= 0xFFFF && QChar(codePoint).isSpace())
        {
            pendingSpace = !normalized.isEmpty();
            continue;
        }
        if (pendingSpace)
        {
            normalized.append(u' ');
            pendingSpace = false;
        }
        normalized.append(QString::fromUcs4(&codePoint, 1));
        if (normalized.size() >= MAX_NORMALIZED_CHARS)
        {
            break;
        }
    }

    return normalized;
}

double RepeatedMessageDetector::compare(const QString &first,
                                        const QString &second)
{
    if (first.isEmpty() && second.isEmpty())
    {
        return 1.0;
    }
    if (first.isEmpty() || second.isEmpty())
    {
        return 0.0;
    }
    if (first == second)
    {
        return 1.0;
    }
    if (first.size() == 1 && second.size() == 1)
    {
        return first == second ? 1.0 : 0.0;
    }
    if (first.size() < 2 || second.size() < 2)
    {
        return 0.0;
    }

    QHash<quint32, int> firstBigrams;
    firstBigrams.reserve(first.size() - 1);
    for (qsizetype i = 0; i < first.size() - 1; ++i)
    {
        const auto key = (quint32(first.at(i).unicode()) << 16U) |
                         quint32(first.at(i + 1).unicode());
        firstBigrams[key] = firstBigrams.value(key) + 1;
    }

    int intersectionSize = 0;
    for (qsizetype i = 0; i < second.size() - 1; ++i)
    {
        const auto key = (quint32(second.at(i).unicode()) << 16U) |
                         quint32(second.at(i + 1).unicode());
        auto count = firstBigrams.value(key);
        if (count > 0)
        {
            firstBigrams[key] = count - 1;
            intersectionSize++;
        }
    }

    return (2.0 * intersectionSize) / double(first.size() + second.size() - 2);
}

bool RepeatedMessageDetector::messagesMatch(const QString &first,
                                            const QString &second,
                                            int sensitivityPercent)
{
    if (first == second)
    {
        return true;
    }

    if (sensitivityPercent >= 100)
    {
        return false;
    }

    return compare(first, second) >= double(sensitivityPercent) / 100.0;
}

int RepeatedMessageDetector::sensitivityToPercent(int sensitivity)
{
    switch (sensitivity)
    {
        case 0:
            return 60;
        case 1:
            return 70;
        case 3:
            return 90;
        case 4:
            return 100;
        case 2:
        default:
            return 80;
    }
}

RepeatedMessageDetector::Entry RepeatedMessageDetector::makeEntry(
    const QString &normalizedMessage, const QString &messageID, qint64 now)
{
    return {
        .normalizedMessage = normalizedMessage,
        .messageID = messageID,
        .repeatCount = 1,
        .missesSinceMatch = 0,
        .expiresAt = now + CACHE_TTL_MS,
    };
}

bool RepeatedMessageDetector::hasSeenMessageID(const UserCache &user,
                                               const QString &messageID)
{
    return std::any_of(user.seenMessages.begin(), user.seenMessages.end(),
                       [&messageID](const SeenMessage &seen) {
                           return seen.messageID == messageID;
                       });
}

void RepeatedMessageDetector::rememberMessageID(UserCache &user,
                                                const QString &messageID,
                                                qint64 now)
{
    user.seenMessages.push_back({messageID, now + CACHE_TTL_MS});
    while (user.seenMessages.size() > MAX_SEEN_MESSAGE_IDS_PER_USER)
    {
        user.seenMessages.pop_front();
    }
}

void RepeatedMessageDetector::cleanupUser(UserCache &user, qint64 now)
{
    if (user.active && user.active->expiresAt <= now)
    {
        user.active.reset();
    }
    if (user.candidate && user.candidate->expiresAt <= now)
    {
        user.candidate.reset();
    }

    user.seenMessages.erase(
        std::remove_if(user.seenMessages.begin(), user.seenMessages.end(),
                       [now](const SeenMessage &seen) {
                           return seen.expiresAt <= now;
                       }),
        user.seenMessages.end());
}

void RepeatedMessageDetector::cleanupChannel(ChannelCache &channel, qint64 now)
{
    for (auto it = channel.begin(); it != channel.end();)
    {
        this->cleanupUser(it.value(), now);

        if (!it.value().active && !it.value().candidate &&
            it.value().seenMessages.isEmpty())
        {
            it = channel.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void RepeatedMessageDetector::enforceChannelLimit(ChannelCache &channel)
{
    while (channel.size() > MAX_USERS_PER_CHANNEL)
    {
        auto oldest = channel.begin();
        for (auto it = channel.begin(); it != channel.end(); ++it)
        {
            if (it.value().lastSeenAt < oldest.value().lastSeenAt)
            {
                oldest = it;
            }
        }
        channel.erase(oldest);
    }
}

}  // namespace chatterino

#include "controllers/commands/builtin/twitch/Pin.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "messages/Message.hpp"
#include "messages/MessageFlag.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "util/Twitch.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUuid>

#include <memory>
#include <optional>

namespace {

using namespace chatterino;
using chatterino::commands::PinDurationParseResult;

constexpr int MAX_TIMED_PIN_DURATION = 30 * 60;
constexpr int SENT_MESSAGE_PIN_TIMEOUT_MS = 3000;

QString usage()
{
    QStringList forms{QStringLiteral("/pin <message-id> [duration]")};

    if (getSettings()->enablePinUserCommand)
    {
        forms.push_back(getSettings()->requireAtForPinUserCommand
                            ? QStringLiteral("/pin @<username> [duration]")
                            : QStringLiteral("/pin <username> [duration]"));
    }

    if (getSettings()->enablePinCommandMessages)
    {
        forms.push_back(QStringLiteral("/pin <message text>"));
    }

    return QStringLiteral("Usage: %1.").arg(forms.join(", "));
}

QString pinTextDisabledMessage()
{
    QStringList alternatives{QStringLiteral("/pin <message-id> [duration]")};
    if (getSettings()->enablePinUserCommand)
    {
        alternatives.push_back(
            getSettings()->requireAtForPinUserCommand
                ? QStringLiteral("/pin @<username> [duration]")
                : QStringLiteral("/pin <username> [duration]"));
    }

    return QStringLiteral("/pin <message text> is disabled. Use %1.")
        .arg(alternatives.join(QStringLiteral(" or ")));
}

bool isMessageId(const QString &text)
{
    return !QUuid(text).isNull();
}

bool isUsernameLike(const QString &text)
{
    static const QRegularExpression usernameRegex(
        QStringLiteral(R"(^[A-Za-z0-9_]{1,25}$)"));

    return usernameRegex.match(text).hasMatch();
}

bool isCurrentUserMessage(const MessagePtr &message, const QString &login,
                          const QString &text)
{
    return message != nullptr && !message->id.isEmpty() &&
           message->loginName.compare(login, Qt::CaseInsensitive) == 0 &&
           message->messageText == text;
}

QString durationHelp()
{
    return QStringLiteral("Use seconds, 5m, 30m, 1h, or indefinite. Durations "
                          "over 30m are pinned indefinitely.");
}

int normalizeParsedPinDuration(qint64 amount, qint64 secondsPerUnit)
{
    if (amount <= 0 || secondsPerUnit <= 0)
    {
        return 0;
    }

    if (amount > MAX_TIMED_PIN_DURATION / secondsPerUnit)
    {
        return 0;
    }

    return static_cast<int>(amount * secondsPerUnit);
}

struct PinUserArgs {
    bool matched = false;
    QString error;
    QString login;
    QString fallbackMessage;
    bool explicitUser = false;
    int durationSeconds = 0;
    int fallbackDurationSeconds = 0;
};

PinUserArgs parsePinUserArgs(const QStringList &words)
{
    if (words.size() < 2)
    {
        return {};
    }

    auto target = words.at(1).trimmed();
    const auto explicitUser = target.startsWith('@');
    stripChannelName(target);

    if (!getSettings()->enablePinUserCommand)
    {
        return {};
    }

    if (getSettings()->requireAtForPinUserCommand && !explicitUser)
    {
        return {};
    }

    if (target.isEmpty() || isMessageId(target))
    {
        return {};
    }

    const auto defaultDuration =
        commands::normalizePinDuration(getSettings()->defaultPinDuration);
    const auto fallbackMessage = words.mid(1).join(' ').trimmed();

    if (words.size() == 2)
    {
        if (!isUsernameLike(target))
        {
            return {};
        }

        return {
            .matched = true,
            .login = target,
            .fallbackMessage = fallbackMessage,
            .explicitUser = explicitUser,
            .durationSeconds = defaultDuration,
            .fallbackDurationSeconds = defaultDuration,
        };
    }

    const auto parsed = commands::parsePinDuration(words.mid(2).join(' '));
    if (parsed.matched)
    {
        if (!parsed.error.isEmpty())
        {
            return {
                .matched = true,
                .error = parsed.error,
            };
        }

        if (!isUsernameLike(target))
        {
            if (explicitUser)
            {
                return {
                    .matched = true,
                    .error = QStringLiteral("Invalid username: %1").arg(target),
                };
            }

            return {};
        }

        return {
            .matched = true,
            .login = target,
            .fallbackMessage = fallbackMessage,
            .explicitUser = explicitUser,
            .durationSeconds = parsed.durationSeconds,
            .fallbackDurationSeconds = defaultDuration,
        };
    }

    if (explicitUser)
    {
        return {
            .matched = true,
            .error = durationHelp(),
        };
    }

    return {};
}

MessagePtr findLatestBufferedMessageFromUser(TwitchChannel *channel,
                                             const QString &login)
{
    if (channel == nullptr)
    {
        return nullptr;
    }

    const auto snapshot = channel->getMessageSnapshot();
    for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it)
    {
        const auto &message = *it;
        if (message == nullptr || message->id.isEmpty())
        {
            continue;
        }
        if (message->flags.hasAny({MessageFlag::System, MessageFlag::Timeout,
                                   MessageFlag::Disabled,
                                   MessageFlag::Whisper}))
        {
            continue;
        }
        if (message->loginName.compare(login, Qt::CaseInsensitive) == 0)
        {
            return message;
        }
    }

    return nullptr;
}

void sendAndPinMessage(TwitchChannel *channel, const QString &messageText,
                       int durationSeconds);

void pinLatestMessageFromUser(TwitchChannel *channel, const QString &login,
                              int durationSeconds,
                              const QString &fallbackMessage,
                              int fallbackDurationSeconds, bool explicitUser)
{
    if (auto message = findLatestBufferedMessageFromUser(channel, login))
    {
        channel->pinMessage(message->id, durationSeconds);
        return;
    }

    const auto weak = channel->weak_from_this();
    TwitchGql::getUserByLogin(
        login, QString{},
        [weak, login, fallbackMessage, fallbackDurationSeconds, explicitUser,
         durationSeconds](std::optional<GqlUser> user) {
            auto shared = std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
            if (!shared)
            {
                return;
            }

            if (!user.has_value())
            {
                if (!explicitUser && !fallbackMessage.isEmpty())
                {
                    if (!getSettings()->enablePinCommandMessages)
                    {
                        shared->addSystemMessage(
                            QStringLiteral("Could not find a user named %1. %2")
                                .arg(login, pinTextDisabledMessage()));
                        return;
                    }

                    sendAndPinMessage(shared.get(), fallbackMessage,
                                      fallbackDurationSeconds);
                    return;
                }

                shared->addSystemMessage(
                    QStringLiteral("Could not find a user named %1.")
                        .arg(login));
                return;
            }

            const auto displayName =
                user->displayName.isEmpty() ? user->login : user->displayName;

            QString authError;
            auto auth = MoltorinoAuth::resolveModerationToken(
                shared->roomId(), shared->getName(), &authError);
            if (!auth.hasToken())
            {
                shared->addSystemMessage(
                    QStringLiteral("Could not search older messages for %1: %2")
                        .arg(displayName, authError));
                return;
            }

            TwitchGql::getLatestModLogMessageBySender(
                shared->roomId(), user->id, auth.token,
                [weak, displayName, durationSeconds,
                 token = auth.token](std::optional<GqlModLogMessage> message) {
                    auto shared =
                        std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
                    if (!shared)
                    {
                        return;
                    }

                    if (!message.has_value())
                    {
                        shared->addSystemMessage(
                            QStringLiteral("Could not find a recent message "
                                           "from %1 to pin.")
                                .arg(displayName));
                        return;
                    }

                    TwitchGql::pinMessage(
                        shared->roomId(), message->id, durationSeconds, token,
                        [] {},
                        [weak, displayName](const QString &error) {
                            auto shared =
                                std::dynamic_pointer_cast<TwitchChannel>(
                                    weak.lock());
                            if (!shared)
                            {
                                return;
                            }

                            shared->addSystemMessage(
                                QStringLiteral(
                                    "Failed to pin latest message from %1: %2")
                                    .arg(displayName,
                                         MoltorinoAuth::normalizeAuthError(
                                             "pinning messages", error)));
                        });
                },
                [weak, displayName](const QString &error) {
                    auto shared =
                        std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
                    if (!shared)
                    {
                        return;
                    }

                    shared->addSystemMessage(
                        QStringLiteral(
                            "Could not search older messages for %1: %2")
                            .arg(displayName,
                                 MoltorinoAuth::normalizeAuthError(
                                     "searching older messages", error)));
                });
        },
        [weak, login](const QString &error) {
            auto shared = std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
            if (!shared)
            {
                return;
            }

            shared->addSystemMessage(
                QStringLiteral("Could not look up user %1: %2")
                    .arg(login, MoltorinoAuth::normalizeAuthError(
                                    "looking up users", error)));
        });
}

void sendAndPinMessage(TwitchChannel *channel, const QString &messageText,
                       int durationSeconds)
{
    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser->isAnon())
    {
        channel->addSystemMessage(
            "You must be logged in to send and pin a message.");
        return;
    }

    const auto login = currentUser->getUserName();
    const auto key = channel->roomId() + '\n' + messageText;

    static QSet<QString> pendingMessages;
    if (pendingMessages.contains(key))
    {
        channel->addSystemMessage(
            "Already waiting for that sent message to appear in chat.");
        return;
    }
    pendingMessages.insert(key);

    auto done = std::make_shared<bool>(false);
    auto connection = std::make_shared<pajlada::Signals::ScopedConnection>();
    auto weakChannel = channel->weak_from_this();

    auto cleanup = [done, connection, key] {
        if (*done)
        {
            return false;
        }

        *done = true;
        *connection = pajlada::Signals::ScopedConnection();
        pendingMessages.remove(key);
        return true;
    };

    *connection = channel->messageAppended.connect(
        [cleanup, weakChannel, login, messageText, durationSeconds](
            MessagePtr &message, auto) mutable {
            if (!isCurrentUserMessage(message, login, messageText))
            {
                return;
            }

            if (!cleanup())
            {
                return;
            }

            auto shared =
                std::dynamic_pointer_cast<TwitchChannel>(weakChannel.lock());
            if (!shared)
            {
                return;
            }

            shared->pinMessage(message->id, durationSeconds);
        });

    QTimer::singleShot(SENT_MESSAGE_PIN_TIMEOUT_MS, [cleanup, weakChannel,
                                                     messageText] mutable {
        if (!cleanup())
        {
            return;
        }

        auto shared =
            std::dynamic_pointer_cast<TwitchChannel>(weakChannel.lock());
        if (!shared)
        {
            return;
        }

        shared->addSystemMessage(
            "Could not find the sent message to pin. Try again.");
    });

    channel->sendMessage(messageText);
}

}  // namespace

namespace chatterino::commands {

PinDurationParseResult parsePinDuration(const QString &text)
{
    const auto duration = text.trimmed().toLower();
    if (duration.isEmpty())
    {
        return {};
    }

    if (duration == "0" || duration == "indefinite" || duration == "forever" ||
        duration == "permanent")
    {
        return {true, {}, 0};
    }

    static const QRegularExpression durationRegex(
        R"(^(\d+)\s*(s|sec|secs|second|seconds|m|min|mins|minute|minutes|h|hr|hrs|hour|hours)?$)",
        QRegularExpression::CaseInsensitiveOption);

    const auto match = durationRegex.match(duration);
    if (!match.hasMatch())
    {
        return {};
    }

    bool ok = false;
    qint64 amount = match.captured(1).toLongLong(&ok);
    if (!ok)
    {
        return {true, {}, 0};
    }

    const auto unit = match.captured(2).toLower();
    if (unit.isEmpty() || unit == "s" || unit == "sec" || unit == "secs" ||
        unit == "second" || unit == "seconds")
    {
        return {true, {}, normalizeParsedPinDuration(amount, 1)};
    }

    if (unit == "m" || unit == "min" || unit == "mins" || unit == "minute" ||
        unit == "minutes")
    {
        return {true, {}, normalizeParsedPinDuration(amount, 60)};
    }

    if (unit == "h" || unit == "hr" || unit == "hrs" || unit == "hour" ||
        unit == "hours")
    {
        return {true, {}, normalizeParsedPinDuration(amount, 60 * 60)};
    }

    return {true, durationHelp(), 0};
}

int normalizePinDuration(int durationSeconds)
{
    if (durationSeconds <= 0 || durationSeconds > MAX_TIMED_PIN_DURATION)
    {
        return 0;
    }

    return durationSeconds;
}

QString pinMessage(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage("/pin only works in Twitch channels.");
        return "";
    }

    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage(usage());
        return "";
    }

    const auto firstArg = ctx.words.at(1).trimmed();
    if (isMessageId(firstArg))
    {
        auto duration = normalizePinDuration(getSettings()->defaultPinDuration);
        if (ctx.words.size() > 2)
        {
            const auto parsed = parsePinDuration(ctx.words.mid(2).join(' '));
            if (!parsed.matched || !parsed.error.isEmpty())
            {
                ctx.channel->addSystemMessage(
                    parsed.error.isEmpty() ? durationHelp() : parsed.error);
                return "";
            }
            duration = parsed.durationSeconds;
        }

        ctx.twitchChannel->pinMessage(firstArg, duration);
        return "";
    }

    const auto userArgs = parsePinUserArgs(ctx.words);
    if (userArgs.matched)
    {
        if (!userArgs.error.isEmpty())
        {
            ctx.channel->addSystemMessage(userArgs.error);
            return "";
        }

        pinLatestMessageFromUser(
            ctx.twitchChannel, userArgs.login, userArgs.durationSeconds,
            userArgs.fallbackMessage, userArgs.fallbackDurationSeconds,
            userArgs.explicitUser);
        return "";
    }

    const auto customMessage = ctx.words.mid(1).join(' ').trimmed();
    if (customMessage.isEmpty())
    {
        ctx.channel->addSystemMessage(usage());
        return "";
    }

    if (!getSettings()->enablePinCommandMessages)
    {
        ctx.channel->addSystemMessage(pinTextDisabledMessage());
        return "";
    }

    sendAndPinMessage(ctx.twitchChannel, customMessage,
                      normalizePinDuration(getSettings()->defaultPinDuration));

    return "";
}

QString unpinMessage(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage("/unpin only works in Twitch channels.");
        return "";
    }

    if (ctx.words.size() > 1)
    {
        ctx.channel->addSystemMessage("Usage: /unpin");
        return "";
    }

    ctx.twitchChannel->unpinMessage();
    return "";
}

}  // namespace chatterino::commands

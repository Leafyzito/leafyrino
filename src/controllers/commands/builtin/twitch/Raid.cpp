#include "controllers/commands/builtin/twitch/Raid.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "util/PostToThread.hpp"
#include "util/Twitch.hpp"

#include <QDateTime>

#include <memory>
#include <utility>

namespace {

using namespace chatterino;

QString formatStartRaidError(HelixStartRaidError error, const QString &message)
{
    QString errorMessage = QString("Failed to start a raid - ");

    using Error = HelixStartRaidError;

    switch (error)
    {
        case Error::UserMissingScope: {
            errorMessage += "Missing required scope. "
                            "Re-login with your "
                            "account and try again.";
        }
        break;

        case Error::UserNotAuthorized: {
            errorMessage += "You must be the broadcaster "
                            "to start a raid.";
        }
        break;

        case Error::CantRaidYourself: {
            errorMessage += "A channel cannot raid itself.";
        }
        break;

        case Error::Ratelimited: {
            errorMessage += "You are being ratelimited "
                            "by Twitch. Try "
                            "again in a few seconds.";
        }
        break;

        case Error::Forwarded: {
            errorMessage += message;
        }
        break;

        case Error::Unknown:
        default: {
            errorMessage += "An unknown error has occurred.";
        }
        break;
    }

    return errorMessage;
}

QString formatCancelRaidError(HelixCancelRaidError error,
                              const QString &message)
{
    QString errorMessage = QString("Failed to cancel the raid - ");

    using Error = HelixCancelRaidError;

    switch (error)
    {
        case Error::UserMissingScope: {
            errorMessage += "Missing required scope. "
                            "Re-login with your "
                            "account and try again.";
        }
        break;

        case Error::UserNotAuthorized: {
            errorMessage += "You must be the broadcaster "
                            "to cancel the raid.";
        }
        break;

        case Error::NoRaidPending: {
            errorMessage += "You don't have an active raid.";
        }
        break;

        case Error::Ratelimited: {
            errorMessage += "You are being ratelimited by Twitch. Try "
                            "again in a few seconds.";
        }
        break;

        case Error::Forwarded: {
            errorMessage += message;
        }
        break;

        case Error::Unknown:
        default: {
            errorMessage += "An unknown error has occurred.";
        }
        break;
    }

    return errorMessage;
}

QString raidControlUsage(const QString &command)
{
    return QString("Usage: \"%1\" - Send the active raid immediately. "
                   "Broadcasters and editors can send the raid.")
        .arg(command);
}

MoltorinoAuthToken resolveRaidAuth(const CommandContext &ctx,
                                   QString *errorMessage = nullptr)
{
    if (ctx.twitchChannel == nullptr)
    {
        return {};
    }

    return MoltorinoAuth::resolveModerationToken(ctx.twitchChannel->roomId(),
                                                 ctx.twitchChannel->getName(),
                                                 errorMessage);
}

QString raidAuthRequiredMessage()
{
    return QStringLiteral(
        "Raid controls need a broadcaster or editor account. Add it in "
        "Settings -> Moltorino -> Authentication, then try again.");
}

QString raidAuthMessage(const QString &authError)
{
    if (authError.isEmpty() ||
        authError.contains("this action", Qt::CaseInsensitive) ||
        authError.contains("raid controls", Qt::CaseInsensitive))
    {
        return raidAuthRequiredMessage();
    }
    return authError;
}

void startRaidWithHelix(const CommandContext &ctx, const QString &target)
{
    const auto roomId = ctx.twitchChannel->roomId();
    getHelix()->getUserByName(
        target,
        [roomId, channel{ctx.channel}](const HelixUser &targetUser) {
            getHelix()->startRaid(
                roomId, targetUser.id, [] {},
                [channel](auto error, auto message) {
                    auto errorMessage = formatStartRaidError(error, message);
                    runInGuiThread([channel, errorMessage] {
                        channel->addSystemMessage(errorMessage);
                    });
                });
        },
        [channel{ctx.channel}, target] {
            runInGuiThread([channel, target] {
                channel->addSystemMessage(
                    QString("Could not look up user: %1. Check the username or "
                            "log in again.")
                        .arg(target));
            });
        });
}

}  // namespace

namespace chatterino::commands {

QString startRaid(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "The /raid command only works in Twitch channels.");
        return "";
    }

    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage(
            "Usage: \"/raid <username>\" - Raid a user. "
            "Broadcasters and editors can start raids.");
        return "";
    }

    auto target = ctx.words.at(1);
    stripChannelName(target);

    QString authError;
    const auto auth = resolveRaidAuth(ctx, &authError);
    if (!auth.hasToken())
    {
        auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
        if (currentUser->isAnon())
        {
            ctx.channel->addSystemMessage(
                "You must be logged in to start a raid!");
            return "";
        }

        if (ctx.twitchChannel->isBroadcaster())
        {
            startRaidWithHelix(ctx, target);
        }
        else
        {
            ctx.channel->addSystemMessage(raidAuthMessage(authError));
        }
        return "";
    }

    const auto sourceLogin = ctx.twitchChannel->getName();
    const auto weak = ctx.twitchChannel->weak_from_this();
    TwitchGql::getRaidChannelIDs(
        sourceLogin, target, auth.token,
        [channel{ctx.channel}, weak, auth](RaidChannelIDs ids) {
            TwitchGql::createRaid(
                ids.sourceId, ids.targetId, auth.token,
                [weak, ids](const QString &raidId) {
                    runInGuiThread([weak, ids, raidId] {
                        auto shared = std::dynamic_pointer_cast<TwitchChannel>(
                            weak.lock());
                        if (!shared)
                        {
                            return;
                        }

                        TwitchChannel::RaidEvent event;
                        event.id = raidId;
                        event.sourceId = ids.sourceId;
                        event.targetId = ids.targetId;
                        event.targetLogin = ids.targetLogin;
                        event.targetDisplayName = ids.targetDisplayName;
                        event.forceRaidNowSeconds = 90;
                        event.receivedAt = QDateTime::currentDateTimeUtc();
                        shared->setActiveRaid(std::move(event));
                    });
                },
                [channel](const QString &error) {
                    runInGuiThread([channel, error] {
                        channel->addSystemMessage(
                            MoltorinoAuth::normalizeAuthError("starting a raid",
                                                              error));
                    });
                });
        },
        [channel{ctx.channel}, target](const QString &error) {
            runInGuiThread([channel, target, error] {
                if (error.contains("channel IDs", Qt::CaseInsensitive))
                {
                    channel->addSystemMessage(
                        QString("Could not look up user: %1. Check the "
                                "username or log in again.")
                            .arg(target));
                    return;
                }

                channel->addSystemMessage(MoltorinoAuth::normalizeAuthError(
                    "starting a raid", error));
            });
        });

    return "";
}

QString cancelRaid(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    const auto commandName =
        ctx.words.isEmpty() ? QStringLiteral("/unraid") : ctx.words.first();

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            QString("The %1 command only works in Twitch channels.")
                .arg(commandName));
        return "";
    }

    if (ctx.words.size() != 1)
    {
        ctx.channel->addSystemMessage(
            QString("Usage: \"%1\" - Cancel the current raid. "
                    "Broadcasters and editors can cancel raids.")
                .arg(commandName));
        return "";
    }

    QString authError;
    const auto auth = resolveRaidAuth(ctx, &authError);
    const auto weak = ctx.twitchChannel->weak_from_this();
    if (auth.hasToken())
    {
        TwitchGql::cancelRaidGql(
            ctx.twitchChannel->roomId(), auth.token,
            [weak] {
                runInGuiThread([weak] {
                    auto shared =
                        std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
                    if (shared)
                    {
                        shared->clearActiveRaid();
                    }
                });
            },
            [channel{ctx.channel}](const QString &error) {
                runInGuiThread([channel, error] {
                    channel->addSystemMessage(MoltorinoAuth::normalizeAuthError(
                        "canceling the raid", error));
                });
            });
        return "";
    }

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser->isAnon())
    {
        ctx.channel->addSystemMessage(
            "You must be logged in to cancel the raid!");
        return "";
    }

    if (!ctx.twitchChannel->isBroadcaster())
    {
        ctx.channel->addSystemMessage(raidAuthMessage(authError));
        return "";
    }

    getHelix()->cancelRaid(
        ctx.twitchChannel->roomId(),
        [weak] {
            runInGuiThread([weak] {
                auto shared =
                    std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
                if (shared)
                {
                    shared->clearActiveRaid();
                }
            });
        },
        [channel{ctx.channel}](auto error, auto message) {
            auto errorMessage = formatCancelRaidError(error, message);
            runInGuiThread([channel, errorMessage] {
                channel->addSystemMessage(errorMessage);
            });
        });

    return "";
}

QString sendRaidNow(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "The /raidsend command only works in Twitch channels.");
        return "";
    }

    if (ctx.words.size() != 1)
    {
        ctx.channel->addSystemMessage(raidControlUsage("/raidsend"));
        return "";
    }

    QString authError;
    const auto auth = resolveRaidAuth(ctx, &authError);
    if (!auth.hasToken())
    {
        ctx.channel->addSystemMessage(raidAuthMessage(authError));
        return "";
    }

    const auto weak = ctx.twitchChannel->weak_from_this();
    TwitchGql::sendRaidNow(
        ctx.twitchChannel->roomId(), auth.token,
        [channel{ctx.channel}, weak] {
            runInGuiThread([channel, weak] {
                auto shared =
                    std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
                if (shared)
                {
                    shared->clearActiveRaid();
                }
                channel->addSystemMessage("Raid sent");
            });
        },
        [channel{ctx.channel}](const QString &error) {
            runInGuiThread([channel, error] {
                channel->addSystemMessage(MoltorinoAuth::normalizeAuthError(
                    "sending the raid", error));
            });
        });

    return "";
}

}  // namespace chatterino::commands

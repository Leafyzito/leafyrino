#include "controllers/commands/builtin/twitch/BlockedTerms.hpp"

#include "common/Channel.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "util/PostToThread.hpp"

#include <algorithm>
#include <optional>

namespace {

using namespace chatterino;

QString usage(const QString &command)
{
    return QStringLiteral("Usage: %1 <phrase>").arg(command);
}

QString phraseFromCommand(const CommandContext &ctx, const QString &command)
{
    QString phrase;
    const auto raw = ctx.rawText.trimmed();
    if (!raw.isEmpty())
    {
        auto rest = raw;
        if (rest.startsWith(command, Qt::CaseInsensitive))
        {
            rest = rest.mid(command.size());
        }
        else
        {
            const auto firstSpace = rest.indexOf(QLatin1Char(' '));
            rest = firstSpace == -1 ? QString() : rest.mid(firstSpace + 1);
        }
        phrase = rest.trimmed();
    }
    else if (ctx.words.size() > 1)
    {
        phrase = ctx.words.mid(1).join(QLatin1Char(' ')).trimmed();
    }

    while (phrase.size() >= 2 && ((phrase.startsWith(QLatin1Char('"')) &&
                                   phrase.endsWith(QLatin1Char('"'))) ||
                                  (phrase.startsWith(QLatin1Char('\'')) &&
                                   phrase.endsWith(QLatin1Char('\'')))))
    {
        phrase = phrase.mid(1, phrase.size() - 2).trimmed();
    }

    return phrase;
}

std::optional<QString> moderationTokenOrWarn(const CommandContext &ctx,
                                             const QString &action)
{
    QString authError;
    const auto auth = ctx.twitchChannel != nullptr
                          ? MoltorinoAuth::resolveModerationToken(
                                ctx.twitchChannel->roomId(),
                                ctx.twitchChannel->getName(), &authError)
                          : MoltorinoAuthToken{};
    if (!auth.hasToken())
    {
        if (ctx.channel != nullptr)
        {
            ctx.channel->addSystemMessage(
                authError.isEmpty() ? MoltorinoAuth::authRequiredMessage(action)
                                    : authError);
        }
        return std::nullopt;
    }

    return auth.token;
}

QString authErrorText(const QString &action, const QString &error)
{
    return MoltorinoAuth::normalizeAuthError(action, error);
}

std::optional<GqlBlockedTerm> findBlockedTerm(
    const QVector<GqlBlockedTerm> &terms, const QString &phrase)
{
    const auto needle = phrase.trimmed();
    auto found =
        std::find_if(terms.begin(), terms.end(), [&](const auto &term) {
            return term.phrase.trimmed() == needle;
        });
    if (found != terms.end())
    {
        return *found;
    }

    found = std::find_if(terms.begin(), terms.end(), [&](const auto &term) {
        return term.phrase.trimmed().compare(needle, Qt::CaseInsensitive) == 0;
    });
    if (found != terms.end())
    {
        return *found;
    }

    return std::nullopt;
}

}  // namespace

namespace chatterino::commands {

QString blockTerm(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "The /blockterm command only works in Twitch channels.");
        return "";
    }

    const auto phrase = phraseFromCommand(ctx, QStringLiteral("/blockterm"));
    if (phrase.isEmpty())
    {
        ctx.channel->addSystemMessage(usage(QStringLiteral("/blockterm")));
        return "";
    }

    const auto token = moderationTokenOrWarn(ctx, "blocking terms");
    if (!token)
    {
        return "";
    }

    TwitchGql::addChannelBlockedTerm(
        ctx.twitchChannel->roomId(), phrase, *token,
        [](GqlAddBlockedTermResult result) {
            (void)result;
        },
        [channel{ctx.channel}](const QString &error) {
            runInGuiThread([channel, error] {
                if (channel != nullptr)
                {
                    channel->addSystemMessage(
                        "Failed to add blocked term: " +
                        authErrorText("blocking terms", error));
                }
            });
        });

    return "";
}

QString unblockTerm(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "The /unblockterm command only works in Twitch channels.");
        return "";
    }

    const auto phrase = phraseFromCommand(ctx, QStringLiteral("/unblockterm"));
    if (phrase.isEmpty())
    {
        ctx.channel->addSystemMessage(usage(QStringLiteral("/unblockterm")));
        return "";
    }

    const auto token = moderationTokenOrWarn(ctx, "removing blocked terms");
    if (!token)
    {
        return "";
    }

    const auto channelId = ctx.twitchChannel->roomId();
    TwitchGql::getChannelBlockedTerms(
        channelId, *token,
        [channel{ctx.channel}, channelId, phrase,
         token = *token](QVector<GqlBlockedTerm> terms) {
            const auto term = findBlockedTerm(terms, phrase);
            if (!term)
            {
                runInGuiThread([channel, phrase] {
                    if (channel != nullptr)
                    {
                        channel->addSystemMessage(
                            QStringLiteral(
                                "Could not find a blocked term matching \"%1\"")
                                .arg(phrase));
                    }
                });
                return;
            }

            TwitchGql::deleteChannelBlockedTerm(
                channelId, term->id, token, [] {},
                [channel](const QString &error) {
                    runInGuiThread([channel, error] {
                        if (channel != nullptr)
                        {
                            channel->addSystemMessage(
                                "Failed to remove blocked term: " +
                                authErrorText("removing blocked terms", error));
                        }
                    });
                });
        },
        [channel{ctx.channel}](const QString &error) {
            runInGuiThread([channel, error] {
                if (channel != nullptr)
                {
                    channel->addSystemMessage(
                        "Failed to fetch blocked terms: " +
                        authErrorText("removing blocked terms", error));
                }
            });
        });

    return "";
}

}  // namespace chatterino::commands

#include "controllers/commands/builtin/twitch/Poll.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "controllers/commands/common/ChannelAction.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/PostToThread.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/dialogs/PollDialog.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#include <QDateTime>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace {

using namespace chatterino;

constexpr auto MIN_POLL_DURATION = std::chrono::seconds(10);
constexpr auto MAX_POLL_DURATION = std::chrono::seconds(1800);

Split *findOpenSplitForChannel(const ChannelPtr &channel)
{
    if (channel == nullptr)
    {
        return nullptr;
    }

    auto *windowManager = getApp()->getWindows();
    if (windowManager == nullptr)
    {
        return nullptr;
    }

    auto *window = windowManager->getLastSelectedWindow();
    if (window == nullptr)
    {
        return nullptr;
    }

    auto *currentPage = dynamic_cast<SplitContainer *>(
        window->getNotebook().getSelectedPage());
    if (currentPage != nullptr)
    {
        if (auto *selectedSplit = currentPage->getSelectedSplit())
        {
            if (selectedSplit->getChannel() == channel)
            {
                return selectedSplit;
            }
        }
    }

    const auto &notebook = window->getNotebook();
    for (int i = 0; i < notebook.getPageCount(); ++i)
    {
        auto *page = dynamic_cast<SplitContainer *>(notebook.getPageAt(i));
        if (page == nullptr)
        {
            continue;
        }

        for (auto *split : page->getSplits())
        {
            if (split != nullptr && split->getChannel() == channel)
            {
                return split;
            }
        }
    }

    return nullptr;
}

QString moltorinoAuthRequiredMessage(const QString &action)
{
    return MoltorinoAuth::authRequiredMessage(action);
}

QString normalizeMoltorinoAuthError(const QString &action,
                                    const QString &error)
{
    return MoltorinoAuth::normalizeAuthError(action, error);
}

std::optional<MoltorinoAuthToken> moltorinoAuthTokenOrWarn(
    const CommandContext &ctx, const QString &action)
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
                authError.isEmpty() ? moltorinoAuthRequiredMessage(action)
                                    : authError);
        }
        return std::nullopt;
    }
    return auth;
}

using PollCallback = std::function<void(
    ChannelPtr, std::shared_ptr<TwitchChannel>, TwitchChannel::PollEvent,
    MoltorinoAuthToken)>;

void withActiveMoltorinoPoll(const CommandContext &ctx, const QString &action,
                             PollCallback callback)
{
    if (ctx.twitchChannel == nullptr)
    {
        const auto err = QStringLiteral(
            "This poll command only works in Twitch channels");
        if (ctx.channel != nullptr)
        {
            ctx.channel->addSystemMessage(err);
        }
        else
        {
            qCWarning(chatterinoCommands) << "Invalid command context:" << err;
        }
        return;
    }

    const auto token = moltorinoAuthTokenOrWarn(ctx, action);
    if (!token)
    {
        return;
    }

    const auto channel = ctx.channel;
    const auto weak = ctx.twitchChannel->weak_from_this();
    const auto channelLogin = ctx.twitchChannel->getName();

    TwitchGql::getActivePoll(
        channelLogin, token->token,
        [channel, weak, action, token = *token,
         callback = std::move(callback)](
            std::optional<TwitchChannel::PollEvent> poll) mutable {
            runInGuiThread(
                [channel, weak, action, token, poll = std::move(poll),
                 callback = std::move(callback)]() mutable {
                    auto shared =
                        std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
                    if (!shared)
                    {
                        return;
                    }

                    if (!poll ||
                        poll->status.compare("ACTIVE", Qt::CaseInsensitive) !=
                            0)
                    {
                        if (channel != nullptr)
                        {
                            channel->addSystemMessage(
                                "Could not find an active poll.");
                        }
                        shared->setActivePoll(std::nullopt);
                        return;
                    }

                    callback(channel, std::move(shared), std::move(*poll),
                             token);
                });
        },
        [channel, action](const QString &error) {
            runInGuiThread([channel, action, error] {
                if (channel != nullptr)
                {
                    channel->addSystemMessage(
                        "Failed to query polls: " +
                        normalizeMoltorinoAuthError(action, error));
                }
            });
        });
}

void clearPollAfterCommand(const ChannelPtr &channel,
                           const std::weak_ptr<Channel> &weak,
                           const QString &message)
{
    runInGuiThread([channel, weak, message] {
        if (auto shared =
                std::dynamic_pointer_cast<TwitchChannel>(weak.lock()))
        {
            shared->setActivePoll(std::nullopt);
        }
        if (channel != nullptr)
        {
            channel->addSystemMessage(message);
        }
    });
}

void finishPollAfterCommand(const ChannelPtr &channel,
                            const std::weak_ptr<Channel> &weak,
                            TwitchChannel::PollEvent poll,
                            const QString &message)
{
    runInGuiThread([channel, weak, poll = std::move(poll), message]() mutable {
        if (auto shared =
                std::dynamic_pointer_cast<TwitchChannel>(weak.lock()))
        {
            poll.status = "COMPLETED";
            poll.remainingDurationMilliseconds = 0;
            poll.endsAt = QDateTime::currentDateTimeUtc();
            shared->setActivePoll(std::move(poll));
        }
        if (channel != nullptr)
        {
            channel->addSystemMessage(message);
        }
    });
}

void showPollCommandError(const ChannelPtr &channel, const QString &prefix,
                          const QString &action, const QString &error)
{
    runInGuiThread([channel, prefix, action, error] {
        if (channel != nullptr)
        {
            channel->addSystemMessage(
                prefix + normalizeMoltorinoAuthError(action, error));
        }
    });
}

}  // namespace

namespace chatterino::commands {

QString createPollHelix(const CommandContext &ctx)
{
    const auto command = QStringLiteral("/poll");
    const auto usage = QStringLiteral(
        R"(Usage: "/poll --title "<title>" --duration <duration>[time unit] --choice "<choice1>" --choice "<choice2>" [options...]" - Creates a poll for users to vote among the defined options. Title may not exceed 60 characters. There must be between two and five poll choices. Duration must be a positive integer; time unit (optional, default=s) must be one of s, m; maximum duration is 30 minutes. Options: --points <points> to allow spending the specified channel points for each additional vote.)");
    const auto action = parseUserParticipationAction(
        ctx, command, usage, MIN_POLL_DURATION, MAX_POLL_DURATION);

    if (!action.has_value())
    {
        if (ctx.channel != nullptr)
        {
            ctx.channel->addSystemMessage(action.error());
        }
        else
        {
            qCWarning(chatterinoCommands)
                << "Error parsing command:" << action.error();
        }
        return "";
    }

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser->isAnon())
    {
        ctx.channel->addSystemMessage(
            "You must be logged in to create a poll!");
        return "";
    }

    const auto &poll = action.value();
    getHelix()->createPoll(
        poll.broadcasterID, poll.title, poll.choices, poll.duration,
        poll.pointsPerVote,
        [channel = ctx.channel, poll] {
            channel->addSystemMessage(
                QString("Created poll: '%1'").arg(poll.title));
        },
        [channel = ctx.channel](const auto &error) {
            channel->addSystemMessage("Failed to create poll - " + error);
        });

    return "";
}

QString createPoll(const CommandContext &ctx)
{
    if (!getSettings()->enablePolls)
    {
        return createPollHelix(ctx);
    }

    if (ctx.twitchChannel == nullptr)
    {
        const auto err = QStringLiteral(
            "The /poll command only works in Twitch channels");
        if (ctx.channel != nullptr)
        {
            ctx.channel->addSystemMessage(err);
        }
        else
        {
            qCWarning(chatterinoCommands) << "Invalid command context:" << err;
        }
        return "";
    }

    PollDialog::showDialog(ctx.twitchChannel,
                           findOpenSplitForChannel(ctx.channel));
    return "";
}

QString endPollHelix(const CommandContext &ctx)
{
    if (ctx.twitchChannel == nullptr)
    {
        const auto err = QStringLiteral(
            "The /endpoll command only works in Twitch channels");
        if (ctx.channel != nullptr)
        {
            ctx.channel->addSystemMessage(err);
        }
        else
        {
            qCWarning(chatterinoCommands) << "Invalid command context:" << err;
        }
        return "";
    }

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser->isAnon())
    {
        ctx.channel->addSystemMessage("You must be logged in to end a poll!");
        return "";
    }

    const auto roomId = ctx.twitchChannel->roomId();
    getHelix()->getPolls(
        roomId, {}, 1, {},
        [channel = ctx.channel, roomId](const auto &result) {
            if (result.polls.empty())
            {
                channel->addSystemMessage("Failed to find any polls");
                return;
            }

            auto poll = result.polls.front();
            if (poll.status != "ACTIVE")
            {
                channel->addSystemMessage("Could not find an active poll");
                return;
            }

            getHelix()->endPoll(
                roomId, poll.id, false,
                [channel](const HelixPoll &data) {
                    HelixPollChoice winner = data.choices.front();
                    int totalVotes = 0;
                    int winnerCount = 0;
                    for (const auto &choice : data.choices)
                    {
                        totalVotes += choice.votes;
                        if (choice.votes > winner.votes)
                        {
                            winner = choice;
                            winnerCount = 1;
                        }
                        else if (choice.votes == winner.votes)
                        {
                            winnerCount++;
                        }
                    }

                    if (totalVotes == 0)
                    {
                        channel->addSystemMessage(
                            QString("Poll ended with zero votes: '%1'")
                                .arg(data.title));
                        return;
                    }

                    if (winnerCount > 1)
                    {
                        channel->addSystemMessage(
                            QString("Poll ended in a draw: '%1'")
                                .arg(data.title));
                        return;
                    }

                    const double percent =
                        100.0 * winner.votes / std::max(totalVotes, 1);

                    channel->addSystemMessage(
                        QString(
                            "Ended poll: '%1' - '%2' won with %3 votes (%4%)")
                            .arg(data.title, winner.title,
                                 QString::number(winner.votes),
                                 QString::number(percent, 'f', 1)));
                },
                [channel](const auto &error) {
                    channel->addSystemMessage("Failed to end the poll - " +
                                              error);
                });
        },
        [channel = ctx.channel](const auto &error) {
            channel->addSystemMessage("Failed to query polls - " + error);
        });

    return "";
}

QString cancelPollHelix(const CommandContext &ctx)
{
    if (ctx.twitchChannel == nullptr)
    {
        const auto err = QStringLiteral(
            "The /cancelpoll command only works in Twitch channels");
        if (ctx.channel != nullptr)
        {
            ctx.channel->addSystemMessage(err);
        }
        else
        {
            qCWarning(chatterinoCommands) << "Invalid command context:" << err;
        }
        return "";
    }

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser->isAnon())
    {
        ctx.channel->addSystemMessage(
            "You must be logged in to cancel a poll!");
        return "";
    }

    const auto roomId = ctx.twitchChannel->roomId();
    getHelix()->getPolls(
        roomId, {}, 1, {},
        [channel = ctx.channel, roomId](const auto &result) {
            if (result.polls.empty())
            {
                channel->addSystemMessage("Failed to find any polls");
                return;
            }

            auto poll = result.polls.front();
            if (poll.status != "ACTIVE")
            {
                channel->addSystemMessage("Could not find an active poll");
                return;
            }

            getHelix()->endPoll(
                roomId, poll.id, true,
                [channel](const HelixPoll &data) {
                    channel->addSystemMessage(
                        QString("Canceled poll: '%1'").arg(data.title));
                },
                [channel](const auto &error) {
                    channel->addSystemMessage("Failed to cancel the poll - " +
                                              error);
                });
        },
        [channel = ctx.channel](const auto &error) {
            channel->addSystemMessage("Failed to query polls - " + error);
        });

    return "";
}

QString endPoll(const CommandContext &ctx)
{
    if (!getSettings()->enablePolls)
    {
        return endPollHelix(ctx);
    }

    withActiveMoltorinoPoll(
        ctx, "ending polls",
        [](ChannelPtr channel, std::shared_ptr<TwitchChannel> twitchChannel,
           TwitchChannel::PollEvent poll, const MoltorinoAuthToken &token) {
            const auto weak = twitchChannel->weak_from_this();
            const auto userId =
                poll.currentUserId.isEmpty() ? token.userId : poll.currentUserId;
            const auto channelLogin = twitchChannel->getName();
            const auto tokenText = token.token;
            const auto originalPoll = poll;
            TwitchGql::terminatePoll(
                poll.id, userId, tokenText,
                [channel, weak, originalPoll]() mutable {
                    finishPollAfterCommand(channel, weak, originalPoll,
                                           "Poll ended");
                },
                [channel, weak, channelLogin, tokenText,
                 originalPoll](const QString &error) {
                    if (error.contains("service error",
                                       Qt::CaseInsensitive))
                    {
                        TwitchGql::getActivePoll(
                            channelLogin, tokenText,
                            [channel, weak,
                             originalPoll](std::optional<TwitchChannel::PollEvent>
                                                refreshedPoll) mutable {
                                if (!refreshedPoll)
                                {
                                    finishPollAfterCommand(
                                        channel, weak, originalPoll,
                                        "Poll ended");
                                    return;
                                }

                                if (refreshedPoll->id == originalPoll.id &&
                                    refreshedPoll->status.compare(
                                        "ACTIVE", Qt::CaseInsensitive) != 0)
                                {
                                    finishPollAfterCommand(
                                        channel, weak, std::move(*refreshedPoll),
                                        "Poll ended");
                                    return;
                                }

                                showPollCommandError(
                                    channel, "Failed to end poll: ",
                                    "ending polls",
                                    "Twitch API Error: service error");
                            },
                            [channel](const QString &refreshError) {
                                showPollCommandError(
                                    channel, "Failed to end poll: ",
                                    "ending polls", refreshError);
                            });
                        return;
                    }

                    showPollCommandError(channel, "Failed to end poll: ",
                                         "ending polls", error);
                });
        });
    return "";
}

QString cancelPoll(const CommandContext &ctx)
{
    if (!getSettings()->enablePolls)
    {
        return cancelPollHelix(ctx);
    }

    withActiveMoltorinoPoll(
        ctx, "deleting polls",
        [](ChannelPtr channel, std::shared_ptr<TwitchChannel> twitchChannel,
           TwitchChannel::PollEvent poll, const MoltorinoAuthToken &token) {
            const auto weak = twitchChannel->weak_from_this();
            TwitchGql::archivePoll(
                poll.id, token.token,
                [channel, weak] {
                    clearPollAfterCommand(channel, weak,
                                          "Poll deleted");
                },
                [channel](const QString &error) {
                    runInGuiThread([channel, error] {
                        if (channel != nullptr)
                        {
                            channel->addSystemMessage(
                                "Failed to delete poll: " +
                                normalizeMoltorinoAuthError("deleting polls",
                                                            error));
                        }
                    });
                });
        });
    return "";
}

}  // namespace chatterino::commands

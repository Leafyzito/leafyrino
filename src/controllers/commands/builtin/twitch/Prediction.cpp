#include "controllers/commands/builtin/twitch/Prediction.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "controllers/commands/common/ChannelAction.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Helpers.hpp"
#include "util/PostToThread.hpp"
#include "widgets/dialogs/PredictionDialog.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#include <QCommandLineParser>
#include <QCursor>
#include <QProcess>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>

namespace {

using namespace chatterino;

constexpr auto MIN_PREDICT_DURATION = std::chrono::seconds(30);
constexpr auto MAX_PREDICT_DURATION = std::chrono::seconds(1800);

QString normalizePredictionCreationError(const QString &error)
{
    const auto lowered = error.toLower();
    if (lowered.contains("forbidden") ||
        (lowered.contains("affiliate") && lowered.contains("partner")))
    {
        return "This channel is not eligible for predictions. Twitch only "
               "allows predictions on Affiliate and Partner channels.";
    }

    return error;
}

Split *findOpenSplitForChannel(const ChannelPtr &channel)
{
    if (!channel)
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

    auto *currentPage =
        dynamic_cast<SplitContainer *>(window->getNotebook().getSelectedPage());
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
            if (split->getChannel() == channel)
            {
                return split;
            }
        }
    }

    return nullptr;
}

QString authErrorText(const QString &action, const QString &error)
{
    return MoltorinoAuth::normalizeAuthError(action, error);
}

std::optional<QString> authTokenOrWarn(const CommandContext &ctx,
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

QString formatPredictionOutcomeOptions(
    const std::vector<TwitchChannel::PredictionOutcome> &outcomes)
{
    QStringList options;
    for (int i = 0; i < static_cast<int>(outcomes.size()); ++i)
    {
        options.push_back(QString("%1: \"%2\"")
                              .arg(QString::number(i + 1), outcomes[i].title));
    }
    return options.join(", ");
}

struct WinnerSelector {
    bool hasValue = false;
    bool byIndex = false;
    bool explicitIndex = false;
    bool explicitTitle = false;
    bool positionalNumeric = false;
    size_t index = 0;
    QString title;
    QString error;
};

WinnerSelector parseWinnerSelector(const CommandContext &ctx)
{
    const auto usage = QStringLiteral(
        R"(Usage: /completeprediction <winner> - Use the one-based number or outcome title. Examples: /completeprediction 1, /completeprediction Yes. Old syntax also works: /completeprediction --choice "<choice>" or /completeprediction --index <index>.)");

    QCommandLineParser parser;
    parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
    parser.setOptionsAfterPositionalArgumentsMode(
        QCommandLineParser::ParseAsOptions);
    QCommandLineOption choiceOption(
        {"c", "choice"}, "The prediction outcome to select as the winner",
        "choice");
    QCommandLineOption indexOption(
        {"i", "index"},
        "The one-based index of the prediction outcome to select as the winner",
        "index");
    parser.addOptions({
        choiceOption,
        indexOption,
    });
    parser.parse(QProcess::splitCommand(ctx.words.join(" ")));

    WinnerSelector selector;
    const bool hasName = parser.isSet(choiceOption);
    const bool hasIndex = parser.isSet(indexOption);
    if (hasName && hasIndex)
    {
        selector.error =
            "You may not specify choice and index simultaneously - " + usage;
        return selector;
    }

    QString rawSelector;
    if (hasName)
    {
        rawSelector = parser.value(choiceOption).trimmed();
        selector.explicitTitle = true;
    }
    else if (hasIndex)
    {
        rawSelector = parser.value(indexOption).trimmed();
        selector.byIndex = true;
        selector.explicitIndex = true;
    }
    else
    {
        rawSelector = ctx.words.mid(1).join(' ').trimmed();
    }

    if (rawSelector.isEmpty())
    {
        return selector;
    }

    if (selector.byIndex)
    {
        bool ok = true;
        selector.index = rawSelector.toULongLong(&ok);
        if (!ok || selector.index == 0)
        {
            selector.error = "Invalid index - " + usage;
            return selector;
        }
        selector.hasValue = true;
        return selector;
    }

    bool numericSelector = false;
    const auto numericIndex = rawSelector.toULongLong(&numericSelector);
    if (!selector.explicitTitle && numericSelector)
    {
        if (numericIndex == 0)
        {
            selector.error = "Invalid index - " + usage;
            return selector;
        }
        selector.byIndex = true;
        selector.positionalNumeric = !selector.explicitIndex;
        selector.index = numericIndex;
        selector.title = rawSelector;
        selector.hasValue = true;
        return selector;
    }

    selector.title = rawSelector;
    selector.hasValue = true;
    return selector;
}

struct WinnerResolution {
    QString outcomeId;
    QString outcomeTitle;
    QString error;
};

WinnerResolution resolveWinnerSelector(
    const TwitchChannel::PredictionEvent &prediction,
    const WinnerSelector &selector)
{
    WinnerResolution resolution;
    const auto options = formatPredictionOutcomeOptions(prediction.outcomes);

    if (!selector.hasValue)
    {
        resolution.error =
            "Choose a winner with /completeprediction <number> or "
            "/completeprediction <outcome>. Outcomes: " +
            options;
        return resolution;
    }

    if (selector.byIndex)
    {
        const bool indexInRange =
            selector.index > 0 && selector.index <= prediction.outcomes.size();
        if (selector.positionalNumeric)
        {
            auto titleMatch = std::find_if(
                prediction.outcomes.begin(), prediction.outcomes.end(),
                [&selector](const TwitchChannel::PredictionOutcome &outcome) {
                    return outcome.title.compare(selector.title,
                                                 Qt::CaseInsensitive) == 0;
                });

            if (titleMatch != prediction.outcomes.end())
            {
                const auto titleIndex =
                    size_t(std::distance(prediction.outcomes.begin(),
                                         titleMatch)) +
                    1;
                if (!indexInRange)
                {
                    resolution.outcomeId = titleMatch->id;
                    resolution.outcomeTitle = titleMatch->title;
                    return resolution;
                }
                if (titleIndex != selector.index)
                {
                    const auto &indexedOutcome =
                        prediction.outcomes[selector.index - 1];
                    resolution.error =
                        QString("Ambiguous winner \"%1\". It could mean "
                                "index #%2 titled \"%3\" or outcome #%4 "
                                "titled \"%5\". Use "
                                "/completeprediction --index %2 or "
                                "/completeprediction --choice \"%1\".")
                            .arg(
                                selector.title, QString::number(selector.index),
                                indexedOutcome.title,
                                QString::number(titleIndex), titleMatch->title);
                    return resolution;
                }
            }
        }

        if (selector.index > prediction.outcomes.size())
        {
            resolution.error =
                QString("Specified index (%1) exceeds the number of outcomes "
                        "(%2). Outcomes: %3")
                    .arg(QString::number(selector.index),
                         QString::number(prediction.outcomes.size()), options);
            return resolution;
        }

        const auto &outcome = prediction.outcomes[selector.index - 1];
        resolution.outcomeId = outcome.id;
        resolution.outcomeTitle = outcome.title;
        return resolution;
    }

    const auto wanted = selector.title.trimmed();
    auto exact = std::find_if(
        prediction.outcomes.begin(), prediction.outcomes.end(),
        [&wanted](const TwitchChannel::PredictionOutcome &outcome) {
            return outcome.title.compare(wanted, Qt::CaseInsensitive) == 0;
        });
    if (exact != prediction.outcomes.end())
    {
        resolution.outcomeId = exact->id;
        resolution.outcomeTitle = exact->title;
        return resolution;
    }

    std::vector<const TwitchChannel::PredictionOutcome *> matches;
    for (const auto &outcome : prediction.outcomes)
    {
        if (outcome.title.startsWith(wanted, Qt::CaseInsensitive))
        {
            matches.push_back(&outcome);
        }
    }

    if (matches.size() == 1)
    {
        resolution.outcomeId = matches.front()->id;
        resolution.outcomeTitle = matches.front()->title;
        return resolution;
    }

    if (matches.size() > 1)
    {
        QStringList matchedTitles;
        for (const auto *match : matches)
        {
            matchedTitles.push_back(QString("\"%1\"").arg(match->title));
        }
        resolution.error =
            "That winner is ambiguous. Matches: " + matchedTitles.join(", ") +
            ". Use the number instead.";
        return resolution;
    }

    resolution.error = "Could not find that winner. Outcomes: " + options;
    return resolution;
}

using PredictionCallback =
    std::function<void(ChannelPtr, std::shared_ptr<TwitchChannel>,
                       TwitchChannel::PredictionEvent, QString)>;

void withActivePrediction(const CommandContext &ctx, const QString &action,
                          PredictionCallback callback)
{
    if (ctx.twitchChannel == nullptr)
    {
        const auto err = QStringLiteral(
            "This prediction command only works in Twitch channels");
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

    const auto token = authTokenOrWarn(ctx, action);
    if (!token)
    {
        return;
    }

    const auto channel = ctx.channel;
    const auto weak = ctx.twitchChannel->weak_from_this();
    const auto channelLogin = ctx.twitchChannel->getName();

    TwitchGql::getActivePrediction(
        channelLogin, *token,
        [channel, weak, action, token = *token, callback = std::move(callback)](
            std::optional<TwitchChannel::PredictionEvent> prediction) mutable {
            runInGuiThread([channel, weak, action, token,
                            prediction = std::move(prediction),
                            callback = std::move(callback)]() mutable {
                auto shared =
                    std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
                if (!shared)
                {
                    return;
                }

                if (!prediction)
                {
                    if (channel != nullptr)
                    {
                        channel->addSystemMessage(
                            "Could not find an open prediction.");
                    }
                    return;
                }

                callback(channel, std::move(shared), std::move(*prediction),
                         token);
            });
        },
        [channel, action](const QString &error) {
            runInGuiThread([channel, action, error] {
                if (channel != nullptr)
                {
                    channel->addSystemMessage("Failed to query predictions: " +
                                              authErrorText(action, error));
                }
            });
        });
}

}  // namespace

namespace chatterino::commands {

QString createPrediction(const CommandContext &ctx)
{
    const auto command = QStringLiteral("/prediction");
    const auto usage = QStringLiteral(
        R"(Usage: "/prediction --title "<title>" --choice "<choice1>" --choice "<choice2>" --duration <duration>[time unit]" - Creates a prediction for users to guess among the defined options. Title may not exceed 45 characters. There must be between two and ten choices. Duration must be a positive integer; time unit (optional, default=s) must be one of s, m; maximum duration is 30 minutes.)");
    const auto action = parseUserParticipationAction(
        ctx, command, usage, MIN_PREDICT_DURATION, MAX_PREDICT_DURATION);

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
            "You must be logged in to create a prediction!");
        return "";
    }

    const auto &data = action.value();
    getHelix()->createPrediction(
        data.broadcasterID, data.title, data.choices, data.duration,
        [channel = ctx.channel, data] {
            channel->addSystemMessage(
                QString("Created prediction: '%1'").arg(data.title));
        },
        [channel = ctx.channel](const auto &error) {
            channel->addSystemMessage("Failed to create prediction - " +
                                      normalizePredictionCreationError(error));
        });

    return "";
}

QString lockPredictionHelix(const CommandContext &ctx)
{
    if (ctx.twitchChannel == nullptr)
    {
        const auto err = QStringLiteral(
            "The /lockprediction command only works in Twitch channels");
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
            "You must be logged in to lock a prediction!");
        return "";
    }

    const auto roomId = ctx.twitchChannel->roomId();
    getHelix()->getPredictions(
        roomId, {}, 1, {},
        [channel = ctx.channel, roomId](const auto &result) {
            if (result.predictions.empty())
            {
                channel->addSystemMessage("Failed to find any predictions");
                return;
            }

            auto prediction = result.predictions.front();
            if (prediction.status == "LOCKED")
            {
                channel->addSystemMessage(
                    "The current prediction is already locked: " +
                    prediction.title);
                return;
            }

            if (prediction.status != "ACTIVE")
            {
                channel->addSystemMessage(
                    "Could not find an active prediction");
                return;
            }

            getHelix()->endPrediction(
                roomId, prediction.id, false, {},
                [channel](const HelixPrediction &data) {
                    int totalPoints = 0;
                    int numUsers = 0;
                    for (const auto &outcome : data.outcomes)
                    {
                        totalPoints += outcome.channelPoints;
                        numUsers += outcome.users;
                    }

                    channel->addSystemMessage(
                        QString("Locked prediction with %1 points wagered by "
                                "%2 users: '%3'")
                            .arg(localizeNumbers(totalPoints),
                                 localizeNumbers(numUsers), data.title));
                },
                [channel](const auto &error) {
                    channel->addSystemMessage("Failed to lock prediction - " +
                                              error);
                });
        },
        [channel = ctx.channel](const auto &error) {
            channel->addSystemMessage("Failed to query predictions - " + error);
        });

    return "";
}

QString cancelPredictionHelix(const CommandContext &ctx)
{
    if (ctx.twitchChannel == nullptr)
    {
        const auto err = QStringLiteral(
            "The /cancelprediction command only works in Twitch channels");
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
            "You must be logged in to cancel a prediction!");
        return "";
    }

    const auto roomId = ctx.twitchChannel->roomId();
    getHelix()->getPredictions(
        roomId, {}, 1, {},
        [channel = ctx.channel, roomId](const auto &result) {
            if (result.predictions.empty())
            {
                channel->addSystemMessage("Failed to find any predictions");
                return;
            }

            auto prediction = result.predictions.front();
            if (prediction.status != "ACTIVE" && prediction.status != "LOCKED")
            {
                channel->addSystemMessage("Could not find an open prediction");
                return;
            }

            getHelix()->endPrediction(
                roomId, prediction.id, true, {},
                [channel](const HelixPrediction &data) {
                    int totalPoints = 0;
                    int numUsers = 0;
                    for (const auto &outcome : data.outcomes)
                    {
                        totalPoints += outcome.channelPoints;
                        numUsers += outcome.users;
                    }

                    channel->addSystemMessage(
                        QString("Refunded %1 points to %2 users for "
                                "prediction: '%3'")
                            .arg(localizeNumbers(totalPoints),
                                 localizeNumbers(numUsers), data.title));
                },
                [channel](const auto &error) {
                    channel->addSystemMessage("Failed to cancel prediction - " +
                                              error);
                });
        },
        [channel = ctx.channel](const auto &error) {
            channel->addSystemMessage("Failed to query predictions - " + error);
        });

    return "";
}

QString completePredictionHelix(const CommandContext &ctx)
{
    const auto usage = QStringLiteral(
        R"(Usage: /completeprediction --choice "<choice>" or /completeprediction --index <index> - Selects a winner for an outstanding prediction. The choice title must exactly match the wording in the prediction. Alternatively, you may specify the one-based index of the winning outcome.)");

    if (ctx.twitchChannel == nullptr)
    {
        const auto err = QStringLiteral(
            "The /completeprediction command only works in Twitch channels");
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

    QCommandLineParser parser;
    parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
    parser.setOptionsAfterPositionalArgumentsMode(
        QCommandLineParser::ParseAsOptions);
    QCommandLineOption choiceOption(
        {"c", "choice"}, "The prediction outcome to select as the winner",
        "choice");
    QCommandLineOption indexOption(
        {"i", "index"},
        "The one-based index of the prediction outcome to select as the winner",
        "index");
    parser.addOptions({
        choiceOption,
        indexOption,
    });
    const auto joined = ctx.words.join(" ");
    parser.parse(QProcess::splitCommand(joined));

    const bool hasName = parser.isSet(choiceOption);
    const bool hasIndex = parser.isSet(indexOption);
    if (hasName && hasIndex)
    {
        ctx.channel->addSystemMessage(
            "You may not specify choice and index simultaneously - " + usage);
        return "";
    }
    if (!hasIndex && !hasName)
    {
        ctx.channel->addSystemMessage(
            "You must specify either choice or index - " + usage);
        return "";
    }

    size_t targetIndex = 0;
    QString targetName;
    if (hasName)
    {
        targetName = parser.value(choiceOption);
    }
    else
    {
        bool ok = true;
        targetIndex = parser.value(indexOption).toULongLong(&ok);
        if (!ok || targetIndex == 0)
        {
            ctx.channel->addSystemMessage("Invalid index - " + usage);
            return "";
        }
    }

    const auto roomId = ctx.twitchChannel->roomId();
    getHelix()->getPredictions(
        roomId, {}, 1, {},
        [channel = ctx.channel, roomId, hasIndex, targetIndex,
         targetName](const auto &queryResult) {
            if (queryResult.predictions.empty())
            {
                channel->addSystemMessage(
                    "You must start a prediction before you can complete one");
                return;
            }

            auto prediction = queryResult.predictions.front();
            if (prediction.status != "ACTIVE" && prediction.status != "LOCKED")
            {
                channel->addSystemMessage(
                    "Could not find an open prediction to complete");
                return;
            }

            auto outcomes = prediction.outcomes;
            QString winnerId = "";
            if (hasIndex)
            {
                auto maxIndex = outcomes.size();
                if (targetIndex > maxIndex)
                {
                    channel->addSystemMessage(
                        QString("Specified index (%1) exceeds the number of "
                                "outcomes (%2)")
                            .arg(QString::number(targetIndex))
                            .arg(QString::number(maxIndex)));
                    return;
                }

                winnerId = outcomes[targetIndex - 1].id;
            }
            else
            {
                for (const auto &outcome : outcomes)
                {
                    if (outcome.title == targetName)
                    {
                        winnerId = outcome.id;
                        break;
                    }
                }

                if (winnerId == "")
                {
                    auto options = std::accumulate(
                        outcomes.begin(), outcomes.end(), QString{},
                        [](const QString &acc,
                           const HelixPredictionOutcome &outcome) {
                            auto title = "'" + outcome.title + "'";
                            return acc.isEmpty() ? title : acc + ", " + title;
                        });
                    channel->addSystemMessage(
                        "Could not find the desired winner. Options include: " +
                        options);
                    return;
                }
            }

            getHelix()->endPrediction(
                roomId, prediction.id, false, winnerId,
                [channel](const HelixPrediction &result) {
                    int totalPoints = 0;
                    HelixPredictionOutcome winner = result.outcomes.front();
                    for (const auto &outcome : result.outcomes)
                    {
                        totalPoints += outcome.channelPoints;
                        if (outcome.id == result.winningOutcomeID)
                        {
                            winner = outcome;
                        }
                    }

                    channel->addSystemMessage(
                        QString("Completed prediction: %1 - '%2' won %3 points "
                                "(%4 profit) to be distributed among %5 users")
                            .arg(result.title, winner.title,
                                 localizeNumbers(totalPoints),
                                 localizeNumbers(totalPoints -
                                                 winner.channelPoints),
                                 localizeNumbers(winner.users)));
                },
                [channel](const auto &error) {
                    channel->addSystemMessage(
                        "Failed to complete prediction - " + error);
                });
        },
        [channel = ctx.channel](const auto &error) {
            channel->addSystemMessage(
                "Failed to query predictions to complete - " + error);
        });

    return "";
}

QString lockPrediction(const CommandContext &ctx)
{
    if (!getSettings()->enablePredictions)
    {
        return lockPredictionHelix(ctx);
    }

    withActivePrediction(
        ctx, "locking predictions",
        [](ChannelPtr channel, std::shared_ptr<TwitchChannel>,
           TwitchChannel::PredictionEvent prediction, const QString &token) {
            if (prediction.status.compare("LOCKED", Qt::CaseInsensitive) == 0)
            {
                if (channel != nullptr)
                {
                    channel->addSystemMessage(
                        "The current prediction is already locked: " +
                        prediction.title);
                }
                return;
            }

            if (prediction.status.compare("ACTIVE", Qt::CaseInsensitive) != 0)
            {
                if (channel != nullptr)
                {
                    channel->addSystemMessage(
                        "Could not find an active prediction.");
                }
                return;
            }

            TwitchGql::lockPrediction(
                prediction.id, token, [] {},
                [channel](const QString &error) {
                    runInGuiThread([channel, error] {
                        if (channel != nullptr)
                        {
                            channel->addSystemMessage(
                                "Failed to lock prediction: " +
                                authErrorText("locking predictions", error));
                        }
                    });
                });
        });
    return "";
}

QString cancelPrediction(const CommandContext &ctx)
{
    if (!getSettings()->enablePredictions)
    {
        return cancelPredictionHelix(ctx);
    }

    withActivePrediction(
        ctx, "deleting predictions",
        [](ChannelPtr channel, std::shared_ptr<TwitchChannel>,
           TwitchChannel::PredictionEvent prediction, const QString &token) {
            if (prediction.status.compare("ACTIVE", Qt::CaseInsensitive) != 0 &&
                prediction.status.compare("LOCKED", Qt::CaseInsensitive) != 0)
            {
                if (channel != nullptr)
                {
                    channel->addSystemMessage(
                        "Could not find an open prediction.");
                }
                return;
            }

            TwitchGql::cancelPrediction(
                prediction.id, token, [] {},
                [channel](const QString &error) {
                    runInGuiThread([channel, error] {
                        if (channel != nullptr)
                        {
                            channel->addSystemMessage(
                                "Failed to delete prediction: " +
                                authErrorText("deleting predictions", error));
                        }
                    });
                });
        });
    return "";
}

QString completePrediction(const CommandContext &ctx)
{
    if (!getSettings()->enablePredictions)
    {
        return completePredictionHelix(ctx);
    }

    const auto selector = parseWinnerSelector(ctx);
    if (!selector.error.isEmpty())
    {
        if (ctx.channel != nullptr)
        {
            ctx.channel->addSystemMessage(selector.error);
        }
        return "";
    }

    withActivePrediction(
        ctx, "completing predictions",
        [selector](ChannelPtr channel, std::shared_ptr<TwitchChannel>,
                   TwitchChannel::PredictionEvent prediction,
                   const QString &token) {
            if (prediction.status.compare("ACTIVE", Qt::CaseInsensitive) != 0 &&
                prediction.status.compare("LOCKED", Qt::CaseInsensitive) != 0)
            {
                if (channel != nullptr)
                {
                    channel->addSystemMessage(
                        "Could not find an open prediction to complete.");
                }
                return;
            }

            const auto winner = resolveWinnerSelector(prediction, selector);
            if (!winner.error.isEmpty())
            {
                if (channel != nullptr)
                {
                    channel->addSystemMessage(winner.error);
                }
                return;
            }

            TwitchGql::resolvePrediction(
                prediction.id, winner.outcomeId, token, [] {},
                [channel](const QString &error) {
                    runInGuiThread([channel, error] {
                        if (channel != nullptr)
                        {
                            channel->addSystemMessage(
                                "Failed to complete prediction: " +
                                authErrorText("completing predictions", error));
                        }
                    });
                });
        });
    return "";
}

QString showPredictions(const CommandContext &ctx)
{
    if (ctx.twitchChannel == nullptr)
    {
        if (ctx.channel != nullptr)
        {
            ctx.channel->addSystemMessage(
                "The /prediction command only works in Twitch channels.");
        }
        return "";
    }
    if (!getSettings()->enablePredictions)
    {
        return createPrediction(ctx);
    }

    auto *split = findOpenSplitForChannel(ctx.channel);
    PredictionDialog::showDialog(ctx.twitchChannel, split);
    return "";
}

}  // namespace chatterino::commands

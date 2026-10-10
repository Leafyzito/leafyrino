#include "controllers/commands/builtin/twitch/ChannelPoints.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "providers/moltorino/MoltorinoFeatureFlags.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/dialogs/RewardQueueDialog.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
#    include "providers/moltorino/MoltorinoAuth.hpp"
#    include "providers/twitch/api/TwitchGql.hpp"
#    include "widgets/dialogs/ChannelPointsChartDialog.hpp"
#    include "widgets/dialogs/ChannelPointsDialog.hpp"
#    include "widgets/splits/SplitInput.hpp"
#endif

#include <algorithm>

namespace {

using namespace chatterino;

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
            if (split != nullptr && split->getChannel() == channel)
            {
                return split;
            }
        }
    }

    return nullptr;
}

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
bool isGigantifyReward(const GqlChannelPointReward &reward)
{
    return reward.isAutomatic &&
           reward.rewardType == QStringLiteral("SEND_GIGANTIFIED_EMOTE") &&
           reward.pricingType.compare(QStringLiteral("BITS"),
                                      Qt::CaseInsensitive) == 0;
}

void addGigantifySystemMessage(const std::weak_ptr<TwitchChannel> &weak,
                               const QString &message)
{
    if (const auto channel = weak.lock())
    {
        channel->addSystemMessage(message);
    }
}

QString friendlyGigantifyError(const QString &error)
{
    const auto normalized =
        MoltorinoAuth::normalizeAuthError("gigantifying a Twitch emote", error);
    const auto upper = normalized.toUpper();
    if (upper.contains(QStringLiteral("BIT")) &&
        (upper.contains(QStringLiteral("INSUFFICIENT")) ||
         upper.contains(QStringLiteral("NOT_ENOUGH")) ||
         upper.contains(QStringLiteral("BALANCE_TOO_LOW"))))
    {
        return QStringLiteral(
            "You do not have enough Bits to Gigantify this emote.");
    }
    if (normalized.trimmed().isEmpty())
    {
        return QStringLiteral("Twitch could not Gigantify that emote.");
    }
    return normalized;
}
#endif

}  // namespace

namespace chatterino::commands {

QString openChannelPointRewards(const CommandContext &ctx)
{
    if (ctx.twitchChannel == nullptr)
    {
        if (ctx.channel != nullptr)
        {
            ctx.channel->addSystemMessage(
                "The /redeem command only works in Twitch channels.");
        }
        return {};
    }

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
    auto *split = findOpenSplitForChannel(ctx.channel);
    auto *input = split == nullptr ? nullptr : &split->getInput();
    ChannelPointsDialog::showDialog(ctx.twitchChannel, input, split);
#else
    if (ctx.channel != nullptr)
    {
        ctx.channel->addSystemMessage(
            "Channel point rewards are not available in this build.");
    }
#endif

    return {};
}

QString openChannelPointsChart(const CommandContext &ctx)
{
    if (ctx.twitchChannel == nullptr)
    {
        if (ctx.channel != nullptr)
        {
            ctx.channel->addSystemMessage(
                "The /pointschart command only works in Twitch channels.");
        }
        return {};
    }

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
    auto *split = findOpenSplitForChannel(ctx.channel);
    ChannelPointsChartDialog::showDialog(ctx.twitchChannel, split);
#else
    if (ctx.channel != nullptr)
    {
        ctx.channel->addSystemMessage(
            "Channel point charts are not available in this build.");
    }
#endif

    return {};
}

QString openRewardQueue(const CommandContext &ctx)
{
    if (ctx.twitchChannel == nullptr)
    {
        if (ctx.channel != nullptr)
        {
            ctx.channel->addSystemMessage(
                "The /rewardqueue command only works in Twitch channels.");
        }
        return {};
    }

    if (!ctx.twitchChannel->hasModRights())
    {
        ctx.channel->addSystemMessage(
            "You must be a moderator to open the reward queue.");
        return {};
    }

    RewardQueueDialog::showDialog(ctx.twitchChannel,
                                  findOpenSplitForChannel(ctx.channel));

    return {};
}

QString sendGigantifiedEmote(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return {};
    }
    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "The /gigantify command only works in Twitch channels.");
        return {};
    }
    if (ctx.words.size() != 2 || ctx.words.at(1).trimmed().isEmpty())
    {
        ctx.channel->addSystemMessage("Usage: /gigantify <Twitch emote>");
        return {};
    }

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
    const auto channel = std::dynamic_pointer_cast<TwitchChannel>(ctx.channel);
    if (!channel)
    {
        ctx.channel->addSystemMessage(
            "The /gigantify command only works in Twitch channels.");
        return {};
    }

    QString authError;
    const auto auth = MoltorinoAuth::resolveCurrentUserToken(&authError);
    if (!auth.hasToken())
    {
        channel->addSystemMessage(authError.isEmpty()
                                      ? MoltorinoAuth::authRequiredMessage(
                                            "gigantifying a Twitch emote")
                                      : authError);
        return {};
    }

    const auto emoteToken = ctx.words.at(1).trimmed();
    const auto weak = std::weak_ptr<TwitchChannel>(channel);
    TwitchGql::getChannelPointRewards(
        channel->getName(), auth.token,
        [weak, token = auth.token, emoteToken](GqlChannelPointRewards rewards) {
            const auto channel = weak.lock();
            if (!channel)
            {
                return;
            }

            const auto rewardIt =
                std::find_if(rewards.rewards.cbegin(), rewards.rewards.cend(),
                             [](const auto &reward) {
                                 return isGigantifyReward(reward);
                             });
            if (rewardIt == rewards.rewards.cend())
            {
                channel->addSystemMessage(
                    "Gigantify is not available in this channel.");
                return;
            }
            if (!rewardIt->isEnabled || !rewardIt->isInStock)
            {
                channel->addSystemMessage(
                    "Gigantify is currently disabled or unavailable in this "
                    "channel.");
                return;
            }
            if (rewardIt->cost <= 0)
            {
                channel->addSystemMessage(
                    "Twitch returned an invalid Gigantify price.");
                return;
            }

            auto channelId = rewards.channelId.trimmed();
            if (channelId.isEmpty())
            {
                channelId = channel->roomId().trimmed();
            }
            if (channelId.isEmpty())
            {
                channel->addSystemMessage(
                    "Wait for the Twitch channel to finish loading, then try "
                    "/gigantify again.");
                return;
            }

            const auto bitsCost = rewardIt->cost;
            TwitchGql::getAvailableGigantifyEmotes(
                channelId, token,
                [weak, channelId, token, emoteToken,
                 bitsCost](QVector<GqlChannelPointEmote> emotes) {
                    if (!weak.lock())
                    {
                        return;
                    }

                    const auto emoteIt =
                        std::find_if(emotes.cbegin(), emotes.cend(),
                                     [&emoteToken](const auto &emote) {
                                         return emote.token == emoteToken &&
                                                !emote.id.isEmpty();
                                     });
                    if (emoteIt == emotes.cend())
                    {
                        addGigantifySystemMessage(
                            weak,
                            QStringLiteral("'%1' is not a Twitch emote "
                                           "you can send in this channel.")
                                .arg(emoteToken));
                        return;
                    }

                    TwitchGql::sendGigantifiedChatEmote(
                        channelId, emoteIt->id, {}, bitsCost, token, [] {},
                        [weak](const QString &error) {
                            addGigantifySystemMessage(
                                weak, friendlyGigantifyError(error));
                        });
                },
                [weak](const QString &error) {
                    addGigantifySystemMessage(
                        weak, MoltorinoAuth::normalizeAuthError(
                                  "loading Twitch emotes", error));
                });
        },
        [weak](const QString &error) {
            addGigantifySystemMessage(weak, MoltorinoAuth::normalizeAuthError(
                                                "loading Gigantify", error));
        });
#else
    ctx.channel->addSystemMessage(
        "Channel point rewards are not available in this build.");
#endif

    return {};
}

}  // namespace chatterino::commands

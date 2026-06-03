#include "controllers/commands/builtin/twitch/ChannelPoints.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "providers/moltorino/MoltorinoFeatureFlags.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
#    include "widgets/dialogs/ChannelPointsDialog.hpp"
#    include "widgets/splits/SplitInput.hpp"
#endif

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

}  // namespace chatterino::commands

#include "controllers/commands/builtin/twitch/Gif.hpp"

#include "Application.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/dialogs/GifPickerDialog.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

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

QString openGifPicker(const CommandContext &ctx)
{
    if (ctx.twitchChannel == nullptr)
    {
        if (ctx.channel != nullptr)
        {
            ctx.channel->addSystemMessage(
                "The /gif command only works in Twitch channels.");
        }
        return {};
    }

    const auto searchTerm = ctx.words.mid(1).join(' ').trimmed();
    GifPickerDialog::showDialog(ctx.twitchChannel, searchTerm,
                                findOpenSplitForChannel(ctx.channel));
    return {};
}

}  // namespace chatterino::commands

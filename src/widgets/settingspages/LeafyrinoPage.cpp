// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/LeafyrinoPage.hpp"

#include "singletons/Settings.hpp"
#include "widgets/BaseWindow.hpp"
#include "widgets/settingspages/GeneralPageView.hpp"
#include "widgets/settingspages/SettingWidget.hpp"

#include <QFrame>
#include <QHBoxLayout>
#include <QVBoxLayout>

namespace chatterino {

LeafyrinoPage::LeafyrinoPage()
{
    auto *outer = new QVBoxLayout;
    auto *inner = new QHBoxLayout;
    auto *view = GeneralPageView::withNavigation(this);
    this->view_ = view;

    inner->addWidget(view);
    auto *frame = new QFrame;
    frame->setLayout(inner);
    outer->addWidget(frame);
    this->setLayout(outer);

    this->initLayout(*view);
}

bool LeafyrinoPage::filterElements(const QString &query)
{
    if (this->view_)
    {
        return this->view_->filterElements(query) || query.isEmpty();
    }

    return false;
}

void LeafyrinoPage::initLayout(GeneralPageView &layout)
{
    auto &s = *getSettings();

    layout.addTitle("Badges");
    SettingWidget::checkbox("Folhinha", s.showBadgesFolhinha)
        ->addKeywords({"folhinha", "folhinhabot"})
        ->setTooltip("FolhinhaBot Plus, Founder, and Supporter badges")
        ->addTo(layout);
    SettingWidget::checkbox("FFZ:AP", s.showBadgesFfzAp)
        ->addKeywords({"ffz", "ffzap", "frankerfacez"})
        ->setTooltip("FFZ:AP supporter and helper badges")
        ->addTo(layout);
    SettingWidget::checkbox("DankChat", s.showBadgesDankChat)
        ->addKeywords({"dankchat", "dank"})
        ->setTooltip("DankChat supporter badges")
        ->addTo(layout);
    SettingWidget::checkbox("Chatsen", s.showBadgesChatsen)
        ->addKeywords({"chatsen"})
        ->setTooltip("Chatsen supporter and developer badges")
        ->addTo(layout);

    layout.addTitle("Client");
    SettingWidget::checkbox("Show select badge button", s.showSelectBadgeButton)
        ->addKeywords({"badge", "select", "button", "client", "picker"})
        ->setTooltip("Show the badge picker button in the chat input bar.")
        ->addTo(layout);

    layout.addTitle("Usercard");
    layout.addDescription("Choose which extra details appear on usercards.");

    SettingWidget::checkbox("Show follower count", s.showUsercardFollowerCount)
        ->addKeywords({"usercard", "follower", "count"})
        ->addTo(layout);
    SettingWidget::checkbox("Show account creation date",
                            s.showUsercardCreatedDate)
        ->addKeywords({"usercard", "created", "account", "date"})
        ->addTo(layout);
    SettingWidget::checkbox("Show last live", s.showUsercardLastLive)
        ->setTooltip("Show when the user was last live. Hover the row to see "
                     "the stream title.")
        ->addKeywords({"usercard", "last", "live", "stream"})
        ->addTo(layout);
    SettingWidget::checkbox("Show user color", s.showUsercardColor)
        ->setTooltip("Show the user's Twitch chat color.")
        ->addKeywords({"usercard", "color", "chat"})
        ->addTo(layout);
    SettingWidget::checkbox("Show 7TV paint", s.showUsercardSevenTVPaint)
        ->setTooltip("Show the user's equipped 7TV paint on the usercard.")
        ->addKeywords({"usercard", "7tv", "seventv", "paint", "cosmetic"})
        ->addTo(layout);
    SettingWidget::checkbox("Show Twitch status", s.showUsercardStatus)
        ->setTooltip(
            "Show whether the user is Staff, Partner, Affiliate, or Regular.")
        ->addKeywords({"usercard", "status", "staff", "partner", "affiliate"})
        ->addTo(layout);
    SettingWidget::checkbox("Show chatter count", s.showUsercardChatterCount)
        ->setTooltip("Show the current chatter count when available.")
        ->addKeywords({"usercard", "chatter", "count"})
        ->addTo(layout);
    SettingWidget::checkbox("Show followage", s.showUsercardFollowage)
        ->addKeywords({"usercard", "followage", "follow"})
        ->addTo(layout);
    SettingWidget::checkbox("Show gift sub gifter", s.showUsercardSubGiftGifter)
        ->setTooltip("When the user has an active gifted subscription in a "
                     "channel, show who gifted it on the usercard.")
        ->addKeywords({"usercard", "gift", "gifter", "subscription", "sub"})
        ->addTo(layout);

    layout.addTitle("Messages per second");
    SettingWidget::checkbox("Show messages-per-second (mps) overlay in splits",
                            s.showSplitMps)
        ->setTooltip("Shows a faint overlay label (e.g. \"12 mps\") with the "
                     "average number of messages per second over the "
                     "selected window.")
        ->addTo(layout);

    SettingWidget::dropdown("MPS averaging window", s.splitMpsWindow)
        ->conditionallyEnabledBy(s.showSplitMps)
        ->setTooltip("Longer windows smooth out Twitch burst delivery so the "
                     "counter does not spike and drop to zero between bursts.")
        ->addTo(layout);

    SettingWidget::dropdown("MPS overlay position", s.splitMpsCorner)
        ->conditionallyEnabledBy(s.showSplitMps)
        ->addTo(layout);

    SettingWidget::checkbox("Show 0 mps", s.showSplitMpsWhenZero)
        ->conditionallyEnabledBy(s.showSplitMps)
        ->addTo(layout);

    layout.addTitle("Miscellaneous");
    SettingWidget::checkbox("Use message colors for tab alerts",
                            s.colorTabHighlightsByMessage)
        ->setTooltip("When a message highlights a tab, use that highlight "
                     "color for the tab alert line.")
        ->addKeywords({"tab", "alert", "highlight", "color"})
        ->addTo(layout);
    SettingWidget::checkbox("Wrap links at breaks", s.wrapLinksAtBreaks)
        ->setTooltip("Let URLs wrap at /, ?, &, #, and = instead of staying "
                     "on one line.")
        ->addKeywords({"url", "link", "wrap", "break"})
        ->addTo(layout);

    layout.addStretch();

    // Invisible element for width
    auto *inv = new BaseWidget(this);
    layout.addWidget(inv);
}

}  // namespace chatterino

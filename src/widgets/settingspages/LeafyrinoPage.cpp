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

    layout.addTitle("YouTube");
    SettingWidget::checkbox("Highlight Super Chats",
                            s.highlightYouTubeSuperChats)
        ->addKeywords(
            {"youtube", "superchat", "super chat", "highlight", "donation"})
        ->setTooltip(
            "Show Super Chats as a colored highlighted row (per tier).")
        ->addTo(layout);
    SettingWidget::checkbox("White username in Super Chats",
                            s.youtubeSuperChatWhiteName)
        ->addKeywords(
            {"youtube", "superchat", "super chat", "username", "white", "name"})
        ->setTooltip(
            "Render the author name in white on the colored Super Chat "
            "row for readability.")
        ->addTo(layout);
    SettingWidget::checkbox("Highlight memberships && gifts",
                            s.highlightYouTubeMemberships)
        ->addKeywords(
            {"youtube", "membership", "member", "gift", "highlight", "green"})
        ->setTooltip(
            "Give new memberships, milestones and gifted memberships a "
            "green highlight.")
        ->addTo(layout);
    SettingWidget::checkbox("Color usernames by role",
                            s.colorYouTubeUsernamesByRole)
        ->addKeywords({"youtube", "username", "color", "role", "moderator",
                       "member", "owner"})
        ->setTooltip("Color only moderators, members and the owner; regular "
                     "chatters stay a neutral grey. Off = all names grey.\n"
                     "Deleted messages follow the global \"hide moderated "
                     "messages\" setting.")
        ->addTo(layout);
    SettingWidget::checkbox("Colorize usernames", s.youtubeColorizeUsernames)
        ->addKeywords(
            {"youtube", "username", "colorize", "color", "nickname", "random"})
        ->setTooltip(
            "Give each YouTube chatter a stable color based on their "
            "channel, like the Twitch colorize option. Roles still use "
            "their role color first.")
        ->addTo(layout);
    SettingWidget::checkbox("Remove @ from usernames", s.youtubeStripAtPrefix)
        ->addKeywords({"youtube", "username", "handle", "at", "prefix"})
        ->setTooltip("Hide the leading @ on YouTube handles in chat. Clicks, "
                     "mentions and the user popup still work with the full "
                     "name.")
        ->addTo(layout);
    SettingWidget::checkbox("Show @username in the split header",
                            s.youtubeSplitHeaderUseHandle)
        ->addKeywords(
            {"youtube", "split", "header", "handle", "username", "channel"})
        ->setTooltip("Show the channel @handle in the split header instead of "
                     "the display name. Hovering always shows the handle.")
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
    SettingWidget::checkbox("Show live viewer count",
                            s.showUsercardLiveViewerCount)
        ->setTooltip("When enabled, replaces the red live dot next to the "
                     "username with the live viewer count.")
        ->addKeywords({"usercard", "live", "viewer", "count", "indicator"})
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
    SettingWidget::checkbox("Show follow button", s.showFollowButtonInUsercard)
        ->setTooltip("Show a follow/unfollow button on usercards. Requires "
                     "Moltorino auth (Settings → Moltorino → Authentication).")
        ->addKeywords({"usercard", "follow", "button"})
        ->addTo(layout);
    SettingWidget::checkbox("Confirm before unfollowing from usercard",
                            s.confirmUnfollowFromUsercard)
        ->conditionallyEnabledBy(s.showFollowButtonInUsercard)
        ->setTooltip("Ask before the usercard follow button unfollows a user. "
                     "The /unfollow command still runs without a prompt.")
        ->addKeywords({"usercard", "follow", "unfollow", "confirm"})
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
    SettingWidget::checkbox("Show full date when hovering timestamps",
                            s.showTimestampDateTooltip)
        ->setTooltip("Show the full message date and time when hovering a "
                     "timestamp. Uses your message timestamp format for the "
                     "time portion.")
        ->addKeywords({"timestamp", "date", "tooltip", "hover"})
        ->addTo(layout);

    layout.addTitle("Follow events");
    layout.addDescription(
        "Shows \"X followed the channel.\" system messages in chat. Only "
        "works in channels where you are the broadcaster or a moderator. "
        "Requires Moltorino auth (Settings → Moltorino → Authentication), "
        "experimental EventSub enabled under Settings → General, and an app "
        "restart after turning EventSub on.");
    SettingWidget::checkbox("Show follow events in chat",
                            s.showFollowEventsInChat)
        ->addKeywords({"follow", "events", "eventsub", "chat", "moderator"})
        ->addTo(layout);

    layout.addStretch();

    // Invisible element for width
    auto *inv = new BaseWidget(this);
    layout.addWidget(inv);
}

}  // namespace chatterino

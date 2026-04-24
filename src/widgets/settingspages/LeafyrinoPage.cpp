#include "widgets/settingspages/LeafyrinoPage.hpp"

#include "singletons/Settings.hpp"
#include "widgets/BaseWindow.hpp"
#include "widgets/settingspages/GeneralPageView.hpp"
#include "widgets/settingspages/SettingWidget.hpp"

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
    SettingWidget::checkbox("Homies", s.showBadgesHomies)
        ->addKeywords({"homies"})
        ->setTooltip("Homies supporter badges and custom badges")
        ->addTo(layout);
    SettingWidget::checkbox("Folhinha", s.showBadgesFolhinha)
        ->addKeywords({"folhinha", "folhinhabot"})
        ->setTooltip("FolhinhaBot Plus and Founder badges")
        ->addTo(layout);

    layout.addTitle("Userinfo popup");
    SettingWidget::checkbox("Show chatters", s.showUserinfoPopupChatters)
        ->addKeywords(
            {"userinfo", "user card", "usercard", "popup", "chatters"})
        ->addTo(layout);
    SettingWidget::checkbox("Show last live date", s.showUserinfoPopupLastLive)
        ->addKeywords(
            {"userinfo", "user card", "usercard", "popup", "last live"})
        ->addTo(layout);
    SettingWidget::checkbox("Show color", s.showUserinfoPopupColor)
        ->addKeywords({"userinfo", "user card", "usercard", "popup", "color"})
        ->addTo(layout);

    layout.addTitle("Panels");
    SettingWidget::checkbox("Show pinned message panel in splits",
                            s.showPinnedMessagePanel)
        ->setTooltip("Polls Twitch for the current moderator-pinned chat "
                     "message and shows it above chat.")
        ->addTo(layout);
    SettingWidget::checkbox("Show active prediction panel in splits",
                            s.showPredictionPanel)
        ->setTooltip("Polls Twitch for an active or locked channel points "
                     "prediction and shows it above chat.")
        ->addTo(layout);

    layout.addTitle("Split performance overlay");
    SettingWidget::checkbox("Show messages-per-second (mps) overlay in splits",
                            s.showSplitMps)
        ->setTooltip("Shows a faint overlay label (e.g. \"12 mps\") of how "
                     "many messages are being sent per second.")
        ->addTo(layout);

    SettingWidget::dropdown("MPS overlay position", s.splitMpsCorner)
        ->conditionallyEnabledBy(s.showSplitMps)
        ->addTo(layout);

    SettingWidget::checkbox("Show 0 mps", s.showSplitMpsWhenZero)
        ->conditionallyEnabledBy(s.showSplitMps)
        ->addTo(layout);

    layout.addTitle("Tabs");
    SettingWidget::checkbox("Always use theme color for tab highlights",
                            s.tabHighlightsUseThemeColor)
        ->setTooltip("Use the theme's default highlighted tab color instead of "
                     "per-highlight colors.")
        ->addTo(layout);

    layout.addStretch();

    // Invisible element for width
    auto *inv = new BaseWidget(this);
    layout.addWidget(inv);
}

}  // namespace chatterino

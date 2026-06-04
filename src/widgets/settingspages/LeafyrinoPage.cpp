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

    layout.addTitle("Messages per second");
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

    layout.addTitle("Miscellaneous");
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

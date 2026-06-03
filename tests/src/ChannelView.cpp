// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/ChannelView.hpp"

#include "mocks/BaseApplication.hpp"
#include "singletons/WindowManager.hpp"
#include "Test.hpp"
#include "widgets/buttons/LabelButton.hpp"
#include "widgets/Scrollbar.hpp"

#include <QLabel>

#include <algorithm>

using namespace chatterino;

namespace {

class MockApplication : public mock::BaseApplication
{
public:
    MockApplication()
        : windowManager(this->args, this->paths_, this->settings, this->theme,
                        this->fonts)
    {
    }

    WindowManager *getWindows() override
    {
        return &this->windowManager;
    }

    WindowManager windowManager;
};

}  // namespace

TEST(ChannelViewTest, GoToBottomGeometryUpdatesWhenScaleChanges)
{
    MockApplication mockApplication;

    ChannelView view(nullptr);
    view.resize(320, 180);

    const auto widgets = view.findChildren<QWidget *>();
    const auto it =
        std::find_if(widgets.begin(), widgets.end(), [](QWidget *w) {
            return dynamic_cast<LabelButton *>(w) != nullptr;
        });
    auto *button =
        it == widgets.end() ? nullptr : dynamic_cast<LabelButton *>(*it);
    ASSERT_NE(button, nullptr);

    auto *scrollbar = view.scrollbar();
    ASSERT_NE(scrollbar, nullptr);

    auto *label = button->findChild<QLabel *>();
    ASSERT_NE(label, nullptr);

    const auto initialHeight = button->height();
    const auto initialScrollbarWidth = scrollbar->width();
    const auto initialLabelFontHeight = label->fontMetrics().height();

    view.setOverrideScale(1.5F);

    EXPECT_GT(button->height(), initialHeight);
    EXPECT_GT(scrollbar->width(), initialScrollbarWidth);
    EXPECT_GT(label->fontMetrics().height(), initialLabelFontHeight);
    EXPECT_EQ(label->font(), button->font());
    EXPECT_EQ(button->height(), int(view.scale() * 26));
    EXPECT_EQ(button->geometry().bottom(), view.rect().bottom());
    EXPECT_EQ(scrollbar->width(), int(view.scale() * 16));
}

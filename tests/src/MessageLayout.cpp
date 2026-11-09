// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/layouts/MessageLayout.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "mocks/BaseApplication.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "Test.hpp"

#include <QDebug>
#include <QString>

#include <memory>

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

    AccountController *getAccounts() override
    {
        return &this->accounts;
    }

    AccountController accounts;
    WindowManager windowManager;
};

constexpr int WIDTH = 300;

class MessageLayoutTest
{
public:
    // "aaaaaaaa bbbbbbbb cccccccc"
    MessageLayoutTest(const QString &text)
    {
        MessageBuilder builder;
        builder.append(
            std::make_unique<TextElement>(text, MessageElementFlag::Text));
        this->layout = std::make_unique<MessageLayout>(builder.release());
        this->layout->layout(this->context(), false);
    }

    MessageLayoutContext context(int width = WIDTH)
    {
        return {
            .messageColors = this->colors,
            .flags = MessageElementFlag::Text,
            .width = width,
            .scale = 1,
            .imageScale = 1,
            .selectedChannel = nullptr,
            .message = *this->layout->getMessage(),
        };
    }

    MockApplication mockApplication;
    MessageColors colors;
    std::unique_ptr<MessageLayout> layout;
};

}  // namespace

TEST(TextElement, BasicCase)
{
    auto test = MessageLayoutTest("abc");

    // Simulate we are clicking on the first word
    auto point = QPoint(WIDTH / 20, test.layout->getHeight() / 2);

    const auto *hoveredElement = test.layout->getElementAt(point);
    ASSERT_NE(hoveredElement, nullptr);

    const auto [wordStart, wordEnd] =
        test.layout->getWordBounds(hoveredElement, point);

    EXPECT_EQ(wordStart, 0);
    EXPECT_EQ(wordEnd, 3);
}

TEST(MessageLayout, DeleteCacheKeepsMetrics)
{
    auto test = MessageLayoutTest("aaaaaaaa bbbbbbbb cccccccc");
    auto &layout = *test.layout;
    ASSERT_TRUE(layout.hasCache());

    const auto height = layout.getHeight();
    const auto width = layout.getWidth();
    const auto lineCount = layout.getLineCount();
    const auto lastCharacterIndex = layout.getLastCharacterIndex();
    const auto point = QPointF(WIDTH / 20, height / 2);
    ASSERT_GT(height, 0);
    ASSERT_NE(layout.getElementAt(point), nullptr);

    QString textBefore;
    layout.addSelectionText(textBefore);
    ASSERT_FALSE(textBefore.isEmpty());

    layout.deleteCache();

    EXPECT_FALSE(layout.hasCache());
    EXPECT_EQ(layout.getHeight(), height);
    EXPECT_EQ(layout.getWidth(), width);
    EXPECT_EQ(layout.getLineCount(), lineCount);
    EXPECT_EQ(layout.getLastCharacterIndex(), lastCharacterIndex);
    EXPECT_EQ(layout.getElementAt(point), nullptr);
    EXPECT_EQ(layout.getSelectionIndex(point), 0);

    QString textWithoutCache;
    layout.addSelectionText(textWithoutCache);
    EXPECT_TRUE(textWithoutCache.isEmpty());

    // nothing changed, but the elements have to be recreated
    EXPECT_TRUE(layout.layout(test.context(), false));
    EXPECT_TRUE(layout.hasCache());
    EXPECT_EQ(layout.getHeight(), height);
    EXPECT_NE(layout.getElementAt(point), nullptr);

    QString textAfter;
    layout.addSelectionText(textAfter);
    EXPECT_EQ(textAfter, textBefore);
}

TEST(MessageLayout, MeasurementDoesNotKeepFreedElements)
{
    auto test = MessageLayoutTest("aaaaaaaa bbbbbbbb cccccccc");
    auto &layout = *test.layout;
    const auto wideHeight = layout.getHeight();
    const auto wideLineCount = layout.getLineCount();

    // elements that exist are kept
    EXPECT_FALSE(layout.layoutForMeasurement(test.context()));
    EXPECT_TRUE(layout.hasCache());

    layout.deleteCache();

    // nothing changed, the cached size is still valid
    EXPECT_FALSE(layout.layoutForMeasurement(test.context()));
    EXPECT_FALSE(layout.hasCache());

    // a narrower view wraps the text, the size is updated without keeping
    // the elements
    EXPECT_TRUE(layout.layoutForMeasurement(test.context(WIDTH / 6)));
    EXPECT_FALSE(layout.hasCache());
    EXPECT_GT(layout.getHeight(), wideHeight);
    EXPECT_GT(layout.getLineCount(), wideLineCount);
    const auto narrowHeight = layout.getHeight();

    EXPECT_TRUE(layout.layout(test.context(WIDTH / 6), false));
    EXPECT_TRUE(layout.hasCache());
    EXPECT_EQ(layout.getHeight(), narrowHeight);
}

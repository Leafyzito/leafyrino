// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/SplitInput.hpp"

#include "common/Literals.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/EmoteController.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "Test.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/helper/ResizingTextEdit.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"

#include <QDebug>
#include <QString>

using namespace chatterino;
using ::testing::Exactly;

namespace {

class TestableSplitInput : public SplitInput
{
public:
    using SplitInput::SplitInput;
    using SplitInput::ui_;
    using SplitInput::updateActionRowCompactness;
};

class MockApplication : public mock::BaseApplication
{
public:
    MockApplication()
        : windowManager(this->args, this->paths_, this->settings, this->theme,
                        this->fonts)
        , commands(this->paths_)
    {
    }

    HotkeyController *getHotkeys() override
    {
        return &this->hotkeys;
    }

    WindowManager *getWindows() override
    {
        return &this->windowManager;
    }

    AccountController *getAccounts() override
    {
        return &this->accounts;
    }

    CommandController *getCommands() override
    {
        return &this->commands;
    }

    EmoteController *getEmotes() override
    {
        return &this->emotes;
    }

    HotkeyController hotkeys;
    WindowManager windowManager;
    AccountController accounts;
    CommandController commands;
    mock::EmoteController emotes;
};

class SplitInputTest
    : public ::testing::TestWithParam<std::tuple<QString, QString>>
{
public:
    SplitInputTest()
        : split(new Split(nullptr))
        , input(this->split)
    {
    }

    MockApplication mockApplication;
    Split *split;
    SplitInput input;
};

}  // namespace

TEST_P(SplitInputTest, Reply)
{
    std::tuple<QString, QString> params = this->GetParam();
    auto [inputText, expected] = params;
    ASSERT_EQ("", this->input.getInputText());
    this->input.setInputText(inputText);
    ASSERT_EQ(inputText, this->input.getInputText());

    auto *message = new Message();
    message->displayName = "forsen";
    auto reply = MessagePtr(message);
    this->input.setReply(reply, {});
    QString actual = this->input.getInputText();
    ASSERT_EQ(expected, actual) << "Input text after setReply should be '"
                                << expected << "', but got '" << actual << "'";
}

INSTANTIATE_TEST_SUITE_P(
    SplitInput, SplitInputTest,
    testing::Values(
        // Ensure message is retained
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "Test message",
            // Expected text after replying to forsen
            "@forsen Test message "),

        // Ensure mention is stripped, no message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen",
            // Expected text after replying to forsen
            "@forsen "),

        // Ensure mention with space is stripped, no message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen ",
            // Expected text after replying to forsen
            "@forsen "),

        // Ensure mention is stripped, retain message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen Test message",
            // Expected text after replying to forsen
            "@forsen Test message "),

        // Ensure mention with comma is stripped, no message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen,",
            // Expected text after replying to forsen
            "@forsen "),

        // Ensure mention with comma is stripped, retain message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen Test message",
            // Expected text after replying to forsen
            "@forsen Test message "),

        // Ensure mention with comma and space is stripped, no message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen, ",
            // Expected text after replying to forsen
            "@forsen "),

        // Ensure it works with no message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "",
            // Expected text after replying to forsen
            "@forsen ")));

TEST(ResizingTextEditTest, HeightForWidthUsesRequestedWidth)
{
    MockApplication mockApplication;

    ResizingTextEdit input;
    input.setPlainText(
        "this is a long enough message to wrap across multiple lines");

    auto wideHeight = static_cast<const QWidget &>(input).heightForWidth(320);
    auto narrowHeight = static_cast<const QWidget &>(input).heightForWidth(120);

    EXPECT_GT(narrowHeight, wideHeight);
}

TEST(SplitInputTest, ChannelPointsChromeStaysCompact)
{
    MockApplication mockApplication;

    auto split = std::make_unique<Split>(nullptr);
    TestableSplitInput input(split.get());

    EXPECT_EQ(input.ui_.channelPointsLabel->font().pointSizeF(),
              input.ui_.textEditLength->font().pointSizeF());
    EXPECT_EQ(input.ui_.channelPointsLabel->toolTip(),
              QString("Channel Points (click to refresh)"));
    EXPECT_FALSE(input.ui_.channelPointsLabel->isVisible());
}

TEST(SplitInputTest, StatusLabelsReserveTextWidth)
{
    MockApplication mockApplication;

    auto split = std::make_unique<Split>(nullptr);
    TestableSplitInput input(split.get());

    input.ui_.textEditLength->setText("123");
    input.setSendWaitStatus("1:23");
    input.updateActionRowCompactness();

    EXPECT_EQ(input.ui_.buttonsRow->indexOf(input.ui_.textEditLength), -1);
    EXPECT_GT(input.ui_.textEditLength->minimumWidth(), 0);
    EXPECT_GT(input.ui_.sendWaitStatus->minimumWidth(), 0);

    input.ui_.textEditLength->clear();
    input.setSendWaitStatus({});
    input.updateActionRowCompactness();

    EXPECT_EQ(input.ui_.textEditLength->minimumWidth(), 0);
    EXPECT_EQ(input.ui_.sendWaitStatus->minimumWidth(), 0);
}

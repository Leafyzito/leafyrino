// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/TwitchChannel.hpp"

#include "controllers/accounts/AccountController.hpp"
#include "controllers/highlights/HighlightController.hpp"
#include "messages/Message.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/Logging.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "providers/twitch/ChannelPointReward.hpp"
#include "providers/twitch/PubSubManager.hpp"
#include "Test.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <vector>

namespace chatterino::detail {

TEST(TwitchChannelDetail_isUnknownCommand, good)
{
    // clang-format off
    std::vector<QString> cases{
        "/me hello",
        ".me hello",
        "/ hello",
        ". hello",
        "/ /hello",
        ". .hello",
        "/ .hello",
        ". /hello",
        ".", // this results in an empty message but not in an error (twitchdev/issues#1019)
        "/me",
        ".me",
        "..",
        "...",
        "....",
        "",
        "foo",
        "a",
        "!",
        ". .",
        ". ..",
        ".. ..",
        ".. .",
        "/ /",
        "/ .",
        ". /",
        ". ./",
        ".. /",
        ".. me",
        ". me",
    };
    // clang-format on

    for (const auto &input : cases)
    {
        ASSERT_FALSE(isUnknownCommand(input))
            << input << " should not be considered an unknown command";
    }
}

TEST(TwitchChannelDetail_isUnknownCommand, bad)
{
    // clang-format off
    std::vector<QString> cases{
        "/badcommand",
        ".badcommand",
        "/badcommand hello",
        ".badcommand hello",
        "/@badcommand hello",
        ".@badcommand hello",
        "/bann username ban reason",
        "/bann username",
        "//",
        "./",
        "./me",
        "./w",
        "/.",
        "/.me",
        "/.w",
        "/,me",
    };
    // clang-format on

    for (const auto &input : cases)
    {
        ASSERT_TRUE(isUnknownCommand(input))
            << input << " should be considered an unknown command";
    }
}

}  // namespace chatterino::detail

namespace chatterino {

class TwitchChannelTestAccess
{
public:
    static void setRoomId(TwitchChannel &channel, const QString &roomId)
    {
        channel.setRoomId(roomId);
    }
};

namespace {

class MockApplication : public mock::BaseApplication
{
public:
    MockApplication()
        : highlights(this->settings, &this->accounts)
        , pubSub("wss://127.0.0.1:9050")
    {
    }

    ILogging *getChatLogger() override
    {
        return &this->logging;
    }

    ITwitchIrcServer *getTwitch() override
    {
        return &this->twitch;
    }

    AccountController *getAccounts() override
    {
        return &this->accounts;
    }

    HighlightController *getHighlights() override
    {
        return &this->highlights;
    }

    PubSub *getTwitchPubSub() override
    {
        return &this->pubSub;
    }

    mock::EmptyLogging logging;
    mock::MockTwitchIrcServer twitch;
    AccountController accounts;
    HighlightController highlights;
    PubSub pubSub;
};

QJsonObject makeChannelPointRedemption(QString redemptionId,
                                       QString cursor = "cursor-1")
{
    QJsonObject redemption{
        {"channel_id", "11148817"},
        {"cursor", cursor},
        {"redeemed_at", "2026-05-28T12:00:00Z"},
        {"user",
         QJsonObject{
             {"id", "129546453"},
             {"login", "nerixyz"},
             {"display_name", "nerixyz"},
         }},
        {"reward",
         QJsonObject{
             {"channel_id", "11148817"},
             {"cost", 1},
             {"id", "31a2344e-0fce-4229-9453-fb2e8b6dd02c"},
             {"is_user_input_required", false},
             {"title", "my reward"},
         }},
    };

    if (!redemptionId.isEmpty())
    {
        redemption.insert("id", redemptionId);
    }

    return redemption;
}

QJsonObject makePollChoice(const QString &id, const QString &title,
                           int votes = 0)
{
    return QJsonObject{
        {"id", id},
        {"title", title},
        {"votes", QJsonObject{{"total", votes}, {"base", votes}}},
    };
}

QJsonObject makePollObject(const QString &id, const QString &title,
                           const QString &status,
                           const QJsonObject &createdBy = {},
                           const QJsonObject &endedBy = {},
                           const QJsonArray &choices = {})
{
    QJsonObject poll{
        {"poll_id", id},
        {"title", title},
        {"status", status},
        {"choices", choices},
    };
    if (!createdBy.isEmpty())
    {
        poll.insert("created_by", createdBy);
    }
    if (!endedBy.isEmpty())
    {
        poll.insert("ended_by", endedBy);
    }
    return poll;
}

QJsonObject makePollUpdatePayload(const QString &type, const QJsonObject &poll)
{
    return QJsonObject{
        {"type", type},
        {"data", QJsonObject{{"poll", poll}}},
    };
}

QJsonArray defaultPollChoices()
{
    return QJsonArray{
        makePollChoice("choice-1", "Red", 3),
        makePollChoice("choice-2", "Blue", 1),
    };
}

QJsonObject makePinnedChatUnpinPayload(const QString &pinId)
{
    return QJsonObject{
        {"type", "unpin-message"},
        {"data",
         QJsonObject{
             {"id", pinId},
             {"unpinned_by",
              QJsonObject{
                  {"display_name", "Moderator"},
                  {"login", "moderator"},
              }},
         }},
    };
}

QJsonObject makePinnedChatPinPayload(const QString &pinId,
                                     const QString &messageId,
                                     const QString &messageText,
                                     const QString &pinnerDisplayName)
{
    return QJsonObject{
        {"type", "pin-message"},
        {"data",
         QJsonObject{
             {"id", pinId},
             {"pinned_by",
              QJsonObject{
                  {"display_name", pinnerDisplayName},
                  {"login", pinnerDisplayName.toLower()},
              }},
             {"message",
              QJsonObject{
                  {"id", messageId},
                  {"content", QJsonObject{{"text", messageText}}},
              }},
         }},
    };
}

QJsonObject makePinnedChatUpdatePayload(const QString &pinId)
{
    return QJsonObject{
        {"type", "update-message"},
        {"data", QJsonObject{{"id", pinId}}},
    };
}

TEST(TwitchChannel, ChannelPointsDefaultToUnknown)
{
    MockApplication app;
    TwitchChannel channel("pajlada");

    EXPECT_EQ(channel.channelPointBalance(), -1);
}

TEST(TwitchChannel, ChannelPointsSignalOnlyOnBalanceChange)
{
    MockApplication app;
    TwitchChannel channel("pajlada");
    int signalCount = 0;
    auto connection = channel.channelPointsChanged.connect([&signalCount] {
        signalCount++;
    });

    channel.setChannelPointBalance(100);
    EXPECT_EQ(channel.channelPointBalance(), 100);
    EXPECT_EQ(signalCount, 1);

    channel.setChannelPointBalance(100);
    EXPECT_EQ(channel.channelPointBalance(), 100);
    EXPECT_EQ(signalCount, 1);

    channel.setChannelPointBalance(125);
    EXPECT_EQ(channel.channelPointBalance(), 125);
    EXPECT_EQ(signalCount, 2);
}

TEST(TwitchChannel, DuplicateChannelPointRewardPubSubMessageIgnored)
{
    MockApplication app;
    TwitchChannel channel("pajlada");

    const auto redemption =
        makeChannelPointRedemption("fd8af65d-3532-4e91-b30e-3995cefe576b");

    channel.addChannelPointReward(ChannelPointReward(redemption));
    channel.addChannelPointReward(ChannelPointReward(redemption));

    EXPECT_EQ(channel.countMessages(), 1);
}

TEST(TwitchChannel, DuplicateChannelPointRewardPubSubMessageIgnoredWithoutId)
{
    MockApplication app;
    TwitchChannel channel("pajlada");

    const auto redemption = makeChannelPointRedemption({}, "cursor-1");

    channel.addChannelPointReward(ChannelPointReward(redemption));
    channel.addChannelPointReward(ChannelPointReward(redemption));

    EXPECT_EQ(channel.countMessages(), 1);
}

TEST(TwitchChannel, ChannelPointsPubSubUpdateRequiresMatchingChannel)
{
    MockApplication app;
    TwitchChannel channel("pajlada");
    TwitchChannelTestAccess::setRoomId(channel, "111");
    channel.setChannelPointBalance(100);

    channel.handleUserPointsUpdate(QJsonObject{
        {"type", "points-spent"},
        {"data", QJsonObject{{"balance", QJsonObject{{"channel_id", "222"},
                                                     {"balance", 90}}}}},
    });

    EXPECT_EQ(channel.channelPointBalance(), 100);
}

TEST(TwitchChannel, ChannelPointsPubSubUpdateIgnoresMalformedPayload)
{
    MockApplication app;
    TwitchChannel channel("pajlada");
    TwitchChannelTestAccess::setRoomId(channel, "111");
    channel.setChannelPointBalance(100);

    channel.handleUserPointsUpdate(QJsonObject{
        {"type", "points-spent"},
        {"data", QJsonObject{{"channel_id", "111"}}},
    });

    EXPECT_EQ(channel.channelPointBalance(), 100);
}

TEST(TwitchChannel, ChannelPointsPubSubUpdateAppliesMatchingBalance)
{
    MockApplication app;
    TwitchChannel channel("pajlada");
    TwitchChannelTestAccess::setRoomId(channel, "111");
    channel.setChannelPointBalance(100);

    channel.handleUserPointsUpdate(QJsonObject{
        {"type", "points-spent"},
        {"data", QJsonObject{{"balance", QJsonObject{{"channel_id", "111"},
                                                     {"balance", 90}}}}},
    });

    EXPECT_EQ(channel.channelPointBalance(), 90);
}

TEST(TwitchChannel, PredictionPubSubUpdateSetsActivePrediction)
{
    MockApplication app;
    TwitchChannel channel("pajlada");

    int signalCount = 0;
    auto connection = channel.predictionChanged.connect([&signalCount] {
        signalCount++;
    });

    channel.handlePredictionUpdate(QJsonObject{
        {"type", "event-created"},
        {"data",
         QJsonObject{
             {"event",
              QJsonObject{
                  {"id", "prediction-1"},
                  {"title", "Will it work?"},
                  {"status", "ACTIVE"},
                  {"prediction_window_seconds", 120},
                  {"created_at", "2026-03-08T12:00:00Z"},
                  {"created_by", QJsonObject{{"display_name", "Moderator"}}},
                  {"locked_by", QJsonObject{}},
                  {"ended_by", QJsonObject{}},
                  {"outcomes",
                   QJsonArray{
                       QJsonObject{{"id", "outcome-1"},
                                   {"title", "Yes"},
                                   {"total_points", 400},
                                   {"total_users", 4}},
                       QJsonObject{{"id", "outcome-2"},
                                   {"title", "No"},
                                   {"total_points", 100},
                                   {"total_users", 1}},
                   }},
              }},
         }},
    });

    auto prediction = *channel.accessPrediction();
    ASSERT_TRUE(prediction.has_value());
    EXPECT_EQ(prediction->id, "prediction-1");
    EXPECT_EQ(prediction->title, "Will it work?");
    EXPECT_EQ(prediction->status, "ACTIVE");
    ASSERT_EQ(prediction->outcomes.size(), 2);
    EXPECT_EQ(prediction->outcomes[0].title, "Yes");
    EXPECT_EQ(prediction->outcomes[1].title, "No");
    EXPECT_EQ(signalCount, 1);
}

TEST(TwitchChannel, PredictionCanceledClearsActivePrediction)
{
    MockApplication app;
    TwitchChannel channel("pajlada");

    TwitchChannel::PredictionEvent prediction;
    prediction.id = "prediction-1";
    prediction.title = "Will it work?";
    prediction.status = "ACTIVE";
    channel.setActivePrediction(prediction);

    channel.handlePredictionUpdate(QJsonObject{
        {"type", "event-canceled"},
        {"data", QJsonObject{}},
    });

    EXPECT_FALSE((*channel.accessPrediction()).has_value());
}

TEST(TwitchChannel, PollPubSubCreateSetsCreatedByName)
{
    MockApplication app;
    TwitchChannel channel("pajlada");

    int signalCount = 0;
    auto connection = channel.pollChanged.connect([&signalCount] {
        signalCount++;
    });

    channel.handlePollUpdate(makePollUpdatePayload(
        "POLL_CREATE",
        makePollObject("poll-1", "Favorite color?", "ACTIVE",
                       QJsonObject{{"display_name", "Moderator"}}, {},
                       defaultPollChoices())));

    const auto poll = *channel.accessPoll();
    ASSERT_TRUE(poll.has_value());
    EXPECT_EQ(poll->id, "poll-1");
    EXPECT_EQ(poll->title, "Favorite color?");
    EXPECT_EQ(poll->status, "ACTIVE");
    EXPECT_EQ(poll->createdByName, "Moderator");
    EXPECT_EQ(signalCount, 1);
}

TEST(TwitchChannel, PollPubSubEndTerminatedSetsEndedByName)
{
    MockApplication app;
    TwitchChannel channel("pajlada");

    TwitchChannel::PollEvent activePoll;
    activePoll.id = "poll-1";
    activePoll.title = "Favorite color?";
    activePoll.status = "ACTIVE";
    activePoll.createdByName = "Moderator";
    activePoll.choices = {
        {.id = "choice-1", .title = "Red", .totalVotes = 3},
        {.id = "choice-2", .title = "Blue", .totalVotes = 1},
    };
    activePoll.totalVotes = 4;
    channel.setActivePoll(activePoll);

    channel.handlePollUpdate(makePollUpdatePayload(
        "POLL_END", makePollObject("poll-1", "Favorite color?", "TERMINATED",
                                   {}, QJsonObject{{"display_name", "Editor"}},
                                   defaultPollChoices())));

    const auto poll = *channel.accessPoll();
    ASSERT_TRUE(poll.has_value());
    EXPECT_EQ(poll->status, "TERMINATED");
    EXPECT_EQ(poll->endedByName, "Editor");
}

TEST(TwitchChannel, PollSystemMessageDedup)
{
    MockApplication app;
    TwitchChannel channel("pajlada");

    const auto payload = makePollUpdatePayload(
        "POLL_CREATE",
        makePollObject("poll-1", "Favorite color?", "ACTIVE",
                       QJsonObject{{"display_name", "Moderator"}}, {},
                       defaultPollChoices()));

    const auto messageCountBefore = channel.getMessageSnapshot().size();

    channel.handlePollUpdate(payload);
    channel.handlePollUpdate(payload);

    const auto messages = channel.getMessageSnapshot();
    ASSERT_EQ(messages.size(), messageCountBefore + 1);
    EXPECT_EQ(messages.back()->messageText,
              "Moderator created a poll: \"Favorite color?\"");
}

TEST(TwitchChannel, PollMergePreservesEndedByName)
{
    MockApplication app;
    TwitchChannel channel("pajlada");

    TwitchChannel::PollEvent poll;
    poll.id = "poll-1";
    poll.title = "Favorite color?";
    poll.status = "TERMINATED";
    poll.createdByName = "Moderator";
    poll.endedByName = "Editor";
    poll.choices = {
        {.id = "choice-1", .title = "Red", .totalVotes = 3},
        {.id = "choice-2", .title = "Blue", .totalVotes = 1},
    };
    poll.totalVotes = 4;
    channel.setActivePoll(poll);

    TwitchChannel::PollEvent update = poll;
    update.endedByName.clear();
    update.choices[0].totalVotes = 5;
    update.totalVotes = 6;
    channel.setActivePoll(update);

    const auto activePoll = *channel.accessPoll();
    ASSERT_TRUE(activePoll.has_value());
    EXPECT_EQ(activePoll->endedByName, "Editor");
    EXPECT_EQ(activePoll->choices[0].totalVotes, 5);
}

TEST(TwitchChannel, PollCreateDefersSystemMessageUntilCreatorKnown)
{
    MockApplication app;
    TwitchChannel channel("pajlada");

    const auto messageCountBefore = channel.getMessageSnapshot().size();

    channel.handlePollUpdate(makePollUpdatePayload(
        "POLL_CREATE", makePollObject("poll-1", "sad", "ACTIVE", {}, {},
                                      defaultPollChoices())));

    EXPECT_EQ(channel.getMessageSnapshot().size(), messageCountBefore);

    TwitchChannel::PollEvent poll;
    poll.id = "poll-1";
    poll.title = "sad";
    poll.status = "ACTIVE";
    poll.createdByName = "Leafyzito";
    poll.choices = {
        {.id = "choice-1", .title = "Red", .totalVotes = 0},
        {.id = "choice-2", .title = "Blue", .totalVotes = 0},
    };
    channel.setActivePoll(poll);

    const auto messages = channel.getMessageSnapshot();
    ASSERT_EQ(messages.size(), messageCountBefore + 1);
    EXPECT_EQ(messages.back()->messageText,
              "Leafyzito created a poll: \"sad\"");
}

TEST(TwitchChannel, PollCompletedUsesTwitchWhenEndedByMissing)
{
    MockApplication app;
    TwitchChannel channel("pajlada");

    channel.handlePollUpdate(makePollUpdatePayload(
        "POLL_END", makePollObject("poll-1", "Favorite color?", "COMPLETED", {},
                                   {}, defaultPollChoices())));

    const auto messages = channel.getMessageSnapshot();
    ASSERT_FALSE(messages.empty());
    EXPECT_EQ(messages.back()->messageText,
              "Twitch ended the poll: \"Red\" won");
}

TEST(TwitchChannel, PollUpdateCompletedEmitsSystemMessage)
{
    MockApplication app;
    TwitchChannel channel("pajlada");

    channel.handlePollUpdate(makePollUpdatePayload(
        "POLL_UPDATE", makePollObject("poll-1", "Favorite color?", "COMPLETED",
                                      {}, {}, defaultPollChoices())));

    const auto messages = channel.getMessageSnapshot();
    ASSERT_FALSE(messages.empty());
    EXPECT_EQ(messages.back()->messageText,
              "Twitch ended the poll: \"Red\" won");
}

TEST(TwitchChannel, PinnedChatPinAddsSystemMessage)
{
    MockApplication app;
    TwitchChannel channel("pajlada");

    const auto messageCountBefore = channel.getMessageSnapshot().size();
    const auto payload =
        makePinnedChatPinPayload("pin-1", "msg-1", "hello chat", "Moderator");

    channel.handlePinnedChatUpdate(payload);

    const auto messages = channel.getMessageSnapshot();
    ASSERT_EQ(messages.size(), messageCountBefore + 1);
    EXPECT_EQ(messages.back()->messageText, "Moderator pinned: \"hello chat\"");
}

TEST(TwitchChannel, DuplicatePinnedChatPinOnlyAddsOneSystemMessage)
{
    MockApplication app;
    TwitchChannel channel("pajlada");

    const auto messageCountBefore = channel.getMessageSnapshot().size();
    const auto payload =
        makePinnedChatPinPayload("pin-1", "msg-1", "hello chat", "Moderator");

    channel.handlePinnedChatUpdate(payload);
    channel.handlePinnedChatUpdate(payload);

    const auto messages = channel.getMessageSnapshot();
    ASSERT_EQ(messages.size(), messageCountBefore + 1);
    EXPECT_EQ(messages.back()->messageText, "Moderator pinned: \"hello chat\"");
}

TEST(TwitchChannel, PinnedChatUpdateDoesNotAddSystemMessage)
{
    MockApplication app;
    TwitchChannel channel("pajlada");

    const auto messageCountBefore = channel.getMessageSnapshot().size();

    channel.handlePinnedChatUpdate(makePinnedChatUpdatePayload("pin-1"));

    EXPECT_EQ(channel.getMessageSnapshot().size(), messageCountBefore);
}

TEST(TwitchChannel, DuplicatePinnedChatUnpinOnlyAddsOneSystemMessage)
{
    MockApplication app;
    TwitchChannel channel("pajlada");

    TwitchChannel::PinnedMessage pin;
    pin.pinId = "pin-1";
    channel.setPinnedMessage(pin);

    int signalCount = 0;
    auto connection = channel.pinnedMessageChanged.connect([&signalCount] {
        signalCount++;
    });

    const auto messageCountBefore = channel.getMessageSnapshot().size();
    const auto payload = makePinnedChatUnpinPayload("pin-1");

    channel.handlePinnedChatUpdate(payload);
    channel.handlePinnedChatUpdate(payload);

    const auto messages = channel.getMessageSnapshot();
    ASSERT_EQ(messages.size(), messageCountBefore + 1);
    EXPECT_EQ(messages.back()->messageText, "Moderator unpinned the message.");
    EXPECT_FALSE((*channel.accessPinnedMessage()).has_value());
    EXPECT_EQ(signalCount, 1);
}

TEST(TwitchChannel, StalePinnedChatUnpinDoesNotClearNewerPin)
{
    MockApplication app;
    TwitchChannel channel("pajlada");

    TwitchChannel::PinnedMessage pin;
    pin.pinId = "pin-new";
    channel.setPinnedMessage(pin);

    const auto messageCountBefore = channel.getMessageSnapshot().size();

    channel.handlePinnedChatUpdate(makePinnedChatUnpinPayload("pin-old"));

    const auto currentPin = *channel.accessPinnedMessage();
    ASSERT_TRUE(currentPin.has_value());
    EXPECT_EQ(currentPin->pinId, "pin-new");
    EXPECT_EQ(channel.getMessageSnapshot().size(), messageCountBefore);
}

}  // namespace

}  // namespace chatterino

// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/highlights/HighlightController.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/highlights/HighlightBadge.hpp"
#include "controllers/highlights/HighlightCheck.hpp"
#include "controllers/highlights/HighlightPhrase.hpp"
#include "controllers/highlights/HighlightResult.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "singletons/Settings.hpp"

namespace {

using namespace chatterino;

auto highlightPhraseCheck(const HighlightPhrase &highlight) -> HighlightCheck
{
    return HighlightCheck{
        [highlight](const auto &args, const auto &twitchBadges,
                    const auto &senderName, const auto &originalMessage,
                    const auto &flags,
                    const auto self) -> std::optional<HighlightResult> {
            (void)args;
            (void)twitchBadges;
            (void)senderName;
            (void)flags;

            if (self)
            {
                return std::nullopt;
            }

            if (!highlight.isMatch(originalMessage))
            {
                return std::nullopt;
            }

            std::optional<QUrl> highlightSoundUrl;
            if (highlight.hasCustomSound())
            {
                highlightSoundUrl = highlight.getSoundUrl();
            }

            return HighlightResult{
                highlight.hasAlert(),       highlight.hasSound(),
                highlightSoundUrl,          highlight.getColor(),
                highlight.showInMentions(),
            };
        }};
}

void rebuildSubscriptionHighlights(Settings &settings,
                                   std::vector<HighlightCheck> &checks)
{
    if (settings.enableSubHighlight)
    {
        auto highlightSound = settings.enableSubHighlightSound.getValue();
        auto highlightAlert = settings.enableSubHighlightTaskbar.getValue();
        auto highlightSoundUrlValue = settings.subHighlightSoundUrl.getValue();
        std::optional<QUrl> highlightSoundUrl;
        if (!highlightSoundUrlValue.isEmpty())
        {
            highlightSoundUrl = highlightSoundUrlValue;
        }

        checks.emplace_back(HighlightCheck{
            [=](const auto &args, const auto &twitchBadges,
                const auto &senderName, const auto &originalMessage,
                const auto &flags,
                const auto self) -> std::optional<HighlightResult> {
                (void)twitchBadges;
                (void)senderName;
                (void)originalMessage;
                (void)flags;
                (void)self;

                if (!args.isSubscriptionMessage)
                {
                    return std::nullopt;
                }

                auto highlightColor =
                    ColorProvider::instance().color(ColorType::Subscription);

                return HighlightResult{
                    highlightAlert, highlightSound, highlightSoundUrl,
                    highlightColor, false,
                };
            }});
    }
}

void rebuildWhisperHighlights(Settings &settings,
                              std::vector<HighlightCheck> &checks)
{
    if (settings.enableWhisperHighlight)
    {
        auto highlightSound = settings.enableWhisperHighlightSound.getValue();
        auto highlightAlert = settings.enableWhisperHighlightTaskbar.getValue();
        auto highlightSoundUrlValue =
            settings.whisperHighlightSoundUrl.getValue();
        std::optional<QUrl> highlightSoundUrl;
        if (!highlightSoundUrlValue.isEmpty())
        {
            highlightSoundUrl = highlightSoundUrlValue;
        }

        checks.emplace_back(HighlightCheck{
            [=](const auto &args, const auto &twitchBadges,
                const auto &senderName, const auto &originalMessage,
                const auto &flags,
                const auto self) -> std::optional<HighlightResult> {
                (void)twitchBadges;
                (void)senderName;
                (void)originalMessage;
                (void)flags;
                (void)self;

                if (!args.isReceivedWhisper)
                {
                    return std::nullopt;
                }

                return HighlightResult{
                    highlightAlert,
                    highlightSound,
                    highlightSoundUrl,
                    ColorProvider::instance().color(ColorType::Whisper),
                    false,
                };
            }});
    }
}

void rebuildReplyThreadHighlight(Settings &settings,
                                 std::vector<HighlightCheck> &checks)
{
    if (settings.enableThreadHighlight)
    {
        auto highlightSound = settings.enableThreadHighlightSound.getValue();
        auto highlightAlert = settings.enableThreadHighlightTaskbar.getValue();
        auto highlightSoundUrlValue =
            settings.threadHighlightSoundUrl.getValue();
        std::optional<QUrl> highlightSoundUrl;
        if (!highlightSoundUrlValue.isEmpty())
        {
            highlightSoundUrl = highlightSoundUrlValue;
        }
        auto highlightInMentions =
            settings.showThreadHighlightInMentions.getValue();
        checks.emplace_back(HighlightCheck{
            [=](const auto &, const auto &, const auto &, const auto &,
                const auto &flags,
                const auto self) -> std::optional<HighlightResult> {
                if (flags.has(MessageFlag::SubscribedThread) && !self)
                {
                    return HighlightResult{
                        highlightAlert,
                        highlightSound,
                        highlightSoundUrl,
                        ColorProvider::instance().color(
                            ColorType::ThreadMessageHighlight),
                        highlightInMentions,
                    };
                }

                return std::nullopt;
            }});
    }
}

void rebuildMessageHighlights(Settings &settings,
                              std::vector<HighlightCheck> &checks)
{
    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    QString currentUsername = currentUser->getUserName();

    if (settings.enableSelfHighlight && !currentUsername.isEmpty() &&
        !currentUser->isAnon())
    {
        HighlightPhrase highlight(
            currentUsername, settings.showSelfHighlightInMentions,
            settings.enableSelfHighlightTaskbar,
            settings.enableSelfHighlightSound, false, false,
            settings.selfHighlightSoundUrl.getValue(),
            ColorProvider::instance().color(ColorType::SelfHighlight));

        checks.emplace_back(highlightPhraseCheck(highlight));
    }

    auto kickUser = getApp()->getAccounts()->kick.current();
    auto kickUsername = kickUser->username();
    if (settings.enableSelfHighlight && !kickUsername.isEmpty() &&
        !kickUser->isAnonymous())
    {
        HighlightPhrase highlight(
            kickUsername, settings.showSelfHighlightInMentions,
            settings.enableSelfHighlightTaskbar,
            settings.enableSelfHighlightSound, false, false,
            settings.selfHighlightSoundUrl.getValue(),
            ColorProvider::instance().color(ColorType::SelfHighlight));

        checks.emplace_back(highlightPhraseCheck(highlight));
    }

    auto messageHighlights = settings.highlightedMessages.readOnly();
    for (const auto &highlight : *messageHighlights)
    {
        checks.emplace_back(highlightPhraseCheck(highlight));
    }

    if (settings.enableAutomodHighlight)
    {
        const auto highlightSound =
            settings.enableAutomodHighlightSound.getValue();
        const auto highlightAlert =
            settings.enableAutomodHighlightTaskbar.getValue();
        const auto highlightSoundUrlValue =
            settings.automodHighlightSoundUrl.getValue();
        auto highlightColor =
            ColorProvider::instance().color(ColorType::AutomodHighlight);

        checks.emplace_back(
            HighlightCheck{[=](const auto &, const auto &, const auto &,
                               const auto &, const auto &flags,
                               const auto) -> std::optional<HighlightResult> {
                if (!flags.has(MessageFlag::AutoModOffendingMessage))
                {
                    return std::nullopt;
                }

                std::optional<QUrl> highlightSoundUrl;
                if (!highlightSoundUrlValue.isEmpty())
                {
                    highlightSoundUrl = highlightSoundUrlValue;
                }

                return HighlightResult{
                    highlightAlert, highlightSound, highlightSoundUrl,
                    highlightColor, false,
                };
            }});
    }
}

void rebuildUserHighlights(Settings &settings,
                           std::vector<HighlightCheck> &checks)
{
    auto userHighlights = settings.highlightedUsers.readOnly();

    if (settings.enableSelfMessageHighlight)
    {
        bool showInMentions = settings.showSelfMessageHighlightInMentions;

        checks.emplace_back(HighlightCheck{
            [showInMentions](
                const auto &args, const auto &twitchBadges,
                const auto &senderName, const auto &originalMessage,
                const auto &flags,
                const auto self) -> std::optional<HighlightResult> {
                (void)args;
                (void)twitchBadges;
                (void)senderName;
                (void)flags;
                (void)originalMessage;

                if (!self)
                {
                    return std::nullopt;
                }

                auto highlightColor = ColorProvider::instance().color(
                    ColorType::SelfMessageHighlight);

                return HighlightResult{false, false, (QUrl) nullptr,
                                       highlightColor, showInMentions};
            }});
    }

    for (const auto &highlight : *userHighlights)
    {
        checks.emplace_back(HighlightCheck{
            [highlight](const auto &args, const auto &twitchBadges,
                        const auto &senderName, const auto &originalMessage,
                        const auto &flags,
                        const auto self) -> std::optional<HighlightResult> {
                (void)args;
                (void)twitchBadges;
                (void)originalMessage;
                (void)flags;
                (void)self;

                if (!highlight.isMatch(senderName))
                {
                    return std::nullopt;
                }

                std::optional<QUrl> highlightSoundUrl;
                if (highlight.hasCustomSound())
                {
                    highlightSoundUrl = highlight.getSoundUrl();
                }

                return HighlightResult{
                    highlight.hasAlert(),       highlight.hasSound(),
                    highlightSoundUrl,          highlight.getColor(),
                    highlight.showInMentions(),
                };
            }});
    }
}

void rebuildBadgeHighlights(Settings &settings,
                            std::vector<HighlightCheck> &checks)
{
    auto badgeHighlights = settings.highlightedBadges.readOnly();

    for (const auto &highlight : *badgeHighlights)
    {
        checks.emplace_back(HighlightCheck{
            [highlight](const auto &args, const auto &twitchBadges,
                        const auto &senderName, const auto &originalMessage,
                        const auto &flags,
                        const auto self) -> std::optional<HighlightResult> {
                (void)args;
                (void)senderName;
                (void)originalMessage;
                (void)flags;
                (void)self;

                for (const TwitchBadge &badge : twitchBadges)
                {
                    if (highlight.isMatch(badge))
                    {
                        std::optional<QUrl> highlightSoundUrl;
                        if (highlight.hasCustomSound())
                        {
                            highlightSoundUrl = highlight.getSoundUrl();
                        }

                        return HighlightResult{
                            highlight.hasAlert(),       highlight.hasSound(),
                            highlightSoundUrl,          highlight.getColor(),
                            highlight.showInMentions(),
                        };
                    }
                }

                return std::nullopt;
            }});
    }
}

}  // namespace

namespace chatterino {

HighlightController::HighlightController(Settings &settings,
                                         AccountController *accounts)
{
    assert(accounts != nullptr);

    this->rebuildListener_.addSetting(settings.enableSelfHighlight);
    this->rebuildListener_.addSetting(settings.enableSelfHighlightSound);
    this->rebuildListener_.addSetting(settings.enableSelfHighlightTaskbar);
    this->rebuildListener_.addSetting(settings.selfHighlightSoundUrl);
    this->rebuildListener_.addSetting(settings.showSelfHighlightInMentions);

    this->rebuildListener_.addSetting(settings.enableWhisperHighlight);
    this->rebuildListener_.addSetting(settings.enableWhisperHighlightSound);
    this->rebuildListener_.addSetting(settings.enableWhisperHighlightTaskbar);
    this->rebuildListener_.addSetting(settings.whisperHighlightSoundUrl);

    this->rebuildListener_.addSetting(settings.enableSubHighlight);
    this->rebuildListener_.addSetting(settings.enableSubHighlightSound);
    this->rebuildListener_.addSetting(settings.enableSubHighlightTaskbar);
    this->rebuildListener_.addSetting(settings.enableSelfMessageHighlight);
    this->rebuildListener_.addSetting(
        settings.showSelfMessageHighlightInMentions);

    this->rebuildListener_.addSetting(settings.subHighlightSoundUrl);

    this->rebuildListener_.addSetting(settings.enableThreadHighlight);
    this->rebuildListener_.addSetting(settings.enableThreadHighlightSound);
    this->rebuildListener_.addSetting(settings.enableThreadHighlightTaskbar);
    this->rebuildListener_.addSetting(settings.threadHighlightSoundUrl);
    this->rebuildListener_.addSetting(settings.showThreadHighlightInMentions);

    this->rebuildListener_.addSetting(settings.enableAutomodHighlight);
    this->rebuildListener_.addSetting(settings.showAutomodInMentions);
    this->rebuildListener_.addSetting(settings.enableAutomodHighlightSound);
    this->rebuildListener_.addSetting(settings.enableAutomodHighlightTaskbar);
    this->rebuildListener_.addSetting(settings.automodHighlightSoundUrl);

    this->rebuildListener_.setCB([this, &settings] {
        qCDebug(chatterinoHighlights)
            << "Rebuild checks because a setting changed";
        this->rebuildChecks(settings);
    });

    this->signalHolder_.managedConnect(
        getSettings()->highlightedBadges.delayedItemsChanged,
        [this, &settings] {
            qCDebug(chatterinoHighlights)
                << "Rebuild checks because highlight badges changed";
            this->rebuildChecks(settings);
        });

    this->signalHolder_.managedConnect(
        getSettings()->highlightedUsers.delayedItemsChanged, [this, &settings] {
            qCDebug(chatterinoHighlights)
                << "Rebuild checks because highlight users changed";
            this->rebuildChecks(settings);
        });

    this->signalHolder_.managedConnect(
        getSettings()->highlightedMessages.delayedItemsChanged,
        [this, &settings] {
            qCDebug(chatterinoHighlights)
                << "Rebuild checks because highlight messages changed";
            this->rebuildChecks(settings);
        });

    this->bConnections.emplace_back(
        accounts->twitch.currentUserChanged.connect([this, &settings] {
            qCDebug(chatterinoHighlights)
                << "Rebuild checks because user swapped accounts";
            this->rebuildChecks(settings);
        }));

    this->signalHolder_.managedConnect(
        accounts->twitch.currentUserNameChanged, [this, &settings] {
            qCDebug(chatterinoHighlights)
                << "Rebuild checks because user name changed";
            this->rebuildChecks(settings);
        });

    this->signalHolder_.managedConnect(
        accounts->kick.currentUserChanged, [this, &settings] {
            qCDebug(chatterinoHighlights)
                << "Rebuild checks because Kick user changed";
            this->rebuildChecks(settings);
        });

    this->rebuildChecks(settings);
}

void HighlightController::rebuildChecks(Settings &settings)
{
    auto checks = this->checks_.access();
    checks->clear();

    rebuildSubscriptionHighlights(settings, *checks);

    rebuildWhisperHighlights(settings, *checks);

    rebuildMessageHighlights(settings, *checks);

    rebuildUserHighlights(settings, *checks);

    rebuildReplyThreadHighlight(settings, *checks);

    rebuildBadgeHighlights(settings, *checks);
}

std::pair<bool, HighlightResult> HighlightController::check(
    const MessageParseArgs &args, const std::vector<TwitchBadge> &twitchBadges,
    const QString &senderName, const QString &originalMessage,
    const MessageFlags &messageFlags, MessagePlatform platform) const
{
    bool highlighted = false;
    auto result = HighlightResult::emptyResult();

    const auto checks = this->checks_.accessConst();

    bool self = false;
    switch (platform)
    {
        case MessagePlatform::AnyOrTwitch: {
            auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
            self = senderName == currentUser->getUserName();
        }
        break;
        case MessagePlatform::Kick: {
            auto kickUser = getApp()->getAccounts()->kick.current();
            self =
                !kickUser->isAnonymous() && senderName == kickUser->username();
        }
        break;
    }

    for (const auto &check : *checks)
    {
        if (auto checkResult = check.cb(args, twitchBadges, senderName,
                                        originalMessage, messageFlags, self);
            checkResult)
        {
            highlighted = true;

            if (checkResult->alert)
            {
                if (!result.alert)
                {
                    result.alert = checkResult->alert;
                }
            }

            if (checkResult->playSound)
            {
                if (!result.playSound)
                {
                    result.playSound = checkResult->playSound;
                }
            }

            if (checkResult->customSoundUrl)
            {
                if (!result.customSoundUrl)
                {
                    result.customSoundUrl = checkResult->customSoundUrl;
                }
            }

            if (checkResult->color)
            {
                if (!result.color)
                {
                    result.color = checkResult->color;
                }
            }

            if (checkResult->showInMentions)
            {
                if (!result.showInMentions)
                {
                    result.showInMentions = checkResult->showInMentions;
                }
            }

            if (result.full())
            {
                break;
            }
        }
    }

    return {highlighted, result};
}

}  // namespace chatterino

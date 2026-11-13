// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/ChatColorCycler.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "util/Twitch.hpp"

#include <QColor>
#include <QRandomGenerator>
#include <QStringList>

#include <chrono>
#include <optional>

namespace chatterino {

namespace {

// keeps fast senders (e.g. /spam) from flooding the color endpoint
constexpr auto MIN_CHANGE_INTERVAL = std::chrono::seconds(1);

// a request that hasn't answered by then shouldn't block cycling forever
constexpr auto IN_FLIGHT_TIMEOUT = std::chrono::seconds(10);

// 15 messages per trip around the color wheel
constexpr int RAINBOW_HUE_STEP = 24;

bool isCustomColor(const QString &color)
{
    return color.startsWith('#');
}

}  // namespace

struct ChatColorCycler::State {
    bool inFlight = false;
    std::chrono::steady_clock::time_point lastChange;
    QString lastColor;
    qsizetype nextIndex = 0;
    int rainbowHue = 0;

    // user IDs twitch refused custom colors for (no Turbo/Prime) or that
    // lack the chat color scope, so we stop asking until a restart
    QString customColorsRejectedFor;
    QString missingScopeFor;

    std::optional<QString> nextColor(ChatColorCycleMode mode,
                                     bool skipCustomColors)
    {
        if (mode == ChatColorCycleMode::Rainbow)
        {
            if (skipCustomColors)
            {
                return std::nullopt;
            }
            this->rainbowHue = (this->rainbowHue + RAINBOW_HUE_STEP) % 360;
            return QColor::fromHsv(this->rainbowHue, 255, 255)
                .name(QColor::HexRgb)
                .toUpper();
        }

        QStringList colors;
        for (const auto &color : getSettings()->chatColorCycleColors.getValue())
        {
            if (!skipCustomColors || !isCustomColor(color))
            {
                colors.append(color);
            }
        }

        if (mode == ChatColorCycleMode::Random)
        {
            colors.removeAll(this->lastColor);
            if (colors.isEmpty())
            {
                return std::nullopt;
            }
            return colors.at(QRandomGenerator::global()->bounded(
                static_cast<int>(colors.size())));
        }

        if (colors.isEmpty())
        {
            return std::nullopt;
        }
        // the list can change between messages
        this->nextIndex %= colors.size();
        return colors.at(this->nextIndex++);
    }
};

ChatColorCycler::ChatColorCycler()
    : state_(std::make_shared<State>())
{
}

void ChatColorCycler::onMessageSent(
    const std::shared_ptr<TwitchChannel> &channel)
{
    auto *settings = getSettings();
    if (!settings->cycleChatColor)
    {
        return;
    }

    auto user = getApp()->getAccounts()->twitch.getCurrent();
    if (user->isAnon())
    {
        return;
    }
    const auto userID = user->getUserId();

    auto &state = *this->state_;
    const auto now = std::chrono::steady_clock::now();
    const auto sinceLastChange = now - state.lastChange;
    if ((state.inFlight && sinceLastChange < IN_FLIGHT_TIMEOUT) ||
        state.missingScopeFor == userID ||
        sinceLastChange < MIN_CHANGE_INTERVAL)
    {
        return;
    }

    auto color = state.nextColor(settings->chatColorCycleMode.getEnum(),
                                 state.customColorsRejectedFor == userID);
    if (!color || *color == state.lastColor)
    {
        return;
    }

    state.inFlight = true;
    state.lastChange = now;

    getHelix()->updateUserChatColor(
        userID, *color,
        [weak = std::weak_ptr(this->state_), user, color = *color] {
            auto state = weak.lock();
            if (!state)
            {
                return;
            }
            state->inFlight = false;
            state->lastColor = color;

            const auto hex =
                isCustomColor(color) ? color : helixColorDisplayHex(color);
            if (!hex.isEmpty())
            {
                user->setColor(QColor(hex));
            }
        },
        [weak = std::weak_ptr(this->state_),
         weakChannel = std::weak_ptr(channel), userID,
         color = *color](auto error, const auto &message) {
            auto state = weak.lock();
            if (!state)
            {
                return;
            }
            state->inFlight = false;

            auto channel = weakChannel.lock();
            switch (error)
            {
                case HelixUpdateUserChatColorError::InvalidColor:
                    if (isCustomColor(color))
                    {
                        state->customColorsRejectedFor = userID;
                        if (channel)
                        {
                            channel->addSystemMessage(
                                QString("Couldn't change your color to %1. "
                                        "Custom colors need Turbo or Prime, "
                                        "so they'll be skipped until you "
                                        "restart Leafyrino.")
                                    .arg(color));
                        }
                        return;
                    }
                    break;

                case HelixUpdateUserChatColorError::UserMissingScope:
                    state->missingScopeFor = userID;
                    if (channel)
                    {
                        channel->addSystemMessage(
                            "Couldn't change your color - Missing required "
                            "scope. Re-login with your account and try "
                            "again.");
                    }
                    return;

                default:
                    break;
            }

            qCWarning(chatterinoTwitch)
                << "Failed to change chat color to" << color << message;
        });
}

}  // namespace chatterino

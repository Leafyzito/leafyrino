// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/FlagsEnum.hpp"
#include "common/UniqueAccess.hpp"
#include "controllers/highlights/HighlightCheck.hpp"
#include "messages/Message.hpp"
#include "singletons/Settings.hpp"

#include <boost/signals2/connection.hpp>
#include <pajlada/settings.hpp>
#include <pajlada/settings/settinglistener.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QColor>
#include <QUrl>

#include <cstdint>
#include <utility>
#include <vector>

namespace chatterino {

class TwitchBadge;
struct MessageParseArgs;
class AccountController;
enum class MessageFlag : std::int64_t;
using MessageFlags = FlagsEnum<MessageFlag>;

class HighlightController final
{
public:
    HighlightController(Settings &settings, AccountController *accounts);

    [[nodiscard]] std::pair<bool, HighlightResult> check(
        const MessageParseArgs &args,
        const std::vector<TwitchBadge> &twitchBadges, const QString &senderName,
        const QString &originalMessage, const MessageFlags &messageFlags,
        MessagePlatform platform = MessagePlatform::AnyOrTwitch) const;

private:

    void rebuildChecks(Settings &settings);

    UniqueAccess<std::vector<HighlightCheck>> checks_;

    pajlada::SettingListener rebuildListener_;
    pajlada::Signals::SignalHolder signalHolder_;
    std::vector<boost::signals2::scoped_connection> bConnections;
};

}

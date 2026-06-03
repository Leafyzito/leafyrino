// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"
#include "messages/Message.hpp"

#include <IrcMessage>
#include <QJsonObject>
#include <QString>
#include <QUrl>

#include <chrono>
#include <memory>
#include <optional>
#include <vector>

namespace chatterino::recentmessages::detail {

std::vector<Communi::IrcMessage *> parseRecentMessages(
    const QJsonObject &jsonRoot);

std::vector<MessagePtr> buildRecentMessages(
    std::vector<Communi::IrcMessage *> &messages, Channel *channel);

QUrl constructRecentMessagesUrl(
    const QString &name, int limit,
    std::optional<std::chrono::time_point<std::chrono::system_clock>> after,
    std::optional<std::chrono::time_point<std::chrono::system_clock>> before);

}

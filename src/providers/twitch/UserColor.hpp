// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/MessageColor.hpp"

#include <QString>

#include <optional>

class QColor;

namespace chatterino {

class IUserDataController;
class ChannelChatters;

namespace twitch {

struct GetUserColorParams {
    QString userLogin;
    QString userID;

    const IUserDataController *userDataController{};
    const ChannelChatters *channelChatters{};

    QColor color;
};

std::optional<MessageColor> getUserColor(const GetUserColorParams &params);

}
}

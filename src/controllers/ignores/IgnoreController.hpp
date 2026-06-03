// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

#include <vector>

namespace chatterino {

class IgnorePhrase;
struct TwitchEmoteOccurrence;

enum class ShowIgnoredUsersMessages { Never, IfModerator, IfBroadcaster };

struct IgnoredMessageParameters {
    QString message;

    QString twitchUserID;
    QString twitchUserLogin;
    bool isMod;
    bool isBroadcaster;
};

bool isIgnoredMessage(IgnoredMessageParameters &&params);

void processIgnorePhrases(const std::vector<IgnorePhrase> &phrases,
                          QString &content,
                          std::vector<TwitchEmoteOccurrence> &twitchEmotes);

}

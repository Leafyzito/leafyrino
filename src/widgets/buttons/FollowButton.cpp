// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/buttons/FollowButton.hpp"

#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/TwitchChannel.hpp"

namespace chatterino {

SvgButton::Src followButtonSource(bool following)
{
    if (following)
    {
        return {
            .dark = ":/buttons/followEnabled-darkMode.svg",
            .light = ":/buttons/followEnabled-lightMode.svg",
        };
    }

    return {
        .dark = ":/buttons/followDisabled-darkMode.svg",
        .light = ":/buttons/followDisabled-lightMode.svg",
    };
}

bool canUseFollowButtonForUser(const QString &userId, const QString &login)
{
    QString ignored;
    const auto auth = MoltorinoAuth::resolveSelectedUserToken(&ignored);
    if (!auth.hasToken())
    {
        return false;
    }

    if (!userId.isEmpty() && !auth.userId.isEmpty() && auth.userId == userId)
    {
        return false;
    }

    return auth.login.isEmpty() ||
           auth.login.compare(login, Qt::CaseInsensitive) != 0;
}

bool canUseFollowButtonForChannel(const TwitchChannel &channel)
{
    return canUseFollowButtonForUser(channel.roomId(), channel.getName());
}

}  // namespace chatterino

#pragma once

#include "messages/Emote.hpp"

#include <QString>

namespace chatterino {

class YouTubeBadges
{
public:
    static EmotePtr moderator();
    static EmotePtr owner();
    static EmotePtr verified();
    static EmotePtr member(const QString &imageUrl, const QString &tooltip);
};

}  // namespace chatterino

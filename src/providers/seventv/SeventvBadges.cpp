#include "providers/seventv/SeventvBadges.hpp"

#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "singletons/Settings.hpp"

namespace chatterino {

QString SeventvBadges::idForBadge(const QJsonObject &badgeJson) const
{
    return badgeJson["id"].toString();
}

EmotePtr SeventvBadges::createBadge(const QString &id,
                                    const QJsonObject &badgeJson) const
{
    auto emote = Emote{

        .name = EmoteName{u"7tv:" % badgeJson["name"].toString()},
        .images = SeventvEmotes::createImageSet(
            badgeJson, !getSettings()->animateSevenTVBadges),
        .tooltip = Tooltip{badgeJson["tooltip"].toString()},
        .homePage = Url{},
        .id = EmoteId{id},
    };

    if (emote.images.getImage1()->isEmpty())
    {
        return nullptr;
    }

    return std::make_shared<const Emote>(std::move(emote));
}

}

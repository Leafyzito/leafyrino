// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/bttv/BttvBadges.hpp"

#include "messages/Emote.hpp"
#include "messages/Image.hpp"

using namespace Qt::Literals::StringLiterals;

namespace chatterino {

QString BttvBadges::idForBadge(const QJsonObject &badgeJson) const
{
    return badgeJson["url"].toString();
}

EmotePtr BttvBadges::createBadge(const QString &id, const QJsonObject &badgeJson) const
{
    const int badgeType = badgeJson["type"].toInt(0);

    QString emoteName;
    QString tooltip;

    switch (badgeType)
    {
        case 1:
            emoteName = u"betterttv:developer"_s;
            tooltip = "BTTV Developer";
            break;
        case 2:
            emoteName = u"betterttv:support-volunteer"_s;
            tooltip = "BTTV Support Volunteer";
            break;
        case 3:
            emoteName = u"betterttv:emote-approver"_s;
            tooltip = "BTTV Emote Approver";
            break;
        case 4:
            emoteName = u"betterttv:translator"_s;
            tooltip = "BTTV Translator";
            break;
        default:
            emoteName = u"betterttv:pro"_s;
            tooltip = "BTTV Pro";
            break;
    }

    auto emote = Emote{
        .name = EmoteName{emoteName},
        .images = ImageSet(Image::fromUrl(Url{id}, 18.0 / 72.0)),
        .tooltip = Tooltip{tooltip},
        .homePage = Url{},
        .id = EmoteId{id},
    };
    return std::make_shared<const Emote>(std::move(emote));
}

}  // namespace chatterino

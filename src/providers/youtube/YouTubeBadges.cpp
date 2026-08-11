#include "providers/youtube/YouTubeBadges.hpp"

#include "messages/Image.hpp"

using namespace Qt::Literals::StringLiterals;

namespace {

using namespace chatterino;

EmotePtr makeAssetBadge(const QString &name, const QString &url18,
                        const QString &url36)
{
    return std::make_shared<const Emote>(Emote{
        .name = {name},
        .images =
            ImageSet{
                Image::fromUrl({url18}, 1.0, {18, 18}),
                Image::fromUrl({url36}, 0.5, {36, 36}),
            },
        .tooltip = Tooltip{name},
    });
}

}  // namespace

namespace chatterino {

EmotePtr YouTubeBadges::moderator()
{
    static EmotePtr badge =
        makeAssetBadge(u"Moderator"_s, u":/badges/youtube-moderator-18.webp"_s,
                       u":/badges/youtube-moderator-36.webp"_s);
    return badge;
}

EmotePtr YouTubeBadges::owner()
{
    static EmotePtr badge =
        makeAssetBadge(u"Owner"_s, u":/badges/youtube-owner-18.webp"_s,
                       u":/badges/youtube-owner-36.webp"_s);
    return badge;
}

EmotePtr YouTubeBadges::verified()
{
    static EmotePtr badge =
        makeAssetBadge(u"Verified"_s, u":/badges/youtube-verified-18.webp"_s,
                       u":/badges/youtube-verified-36.webp"_s);
    return badge;
}

EmotePtr YouTubeBadges::member(const QString &imageUrl, const QString &tooltip)
{
    auto name = tooltip.isEmpty() ? u"Member"_s : tooltip;
    return std::make_shared<const Emote>(Emote{
        .name = {name},
        .images = ImageSet{Image::fromAutoscaledUrl({imageUrl}, 18)},
        .tooltip = Tooltip{name},
    });
}

}  // namespace chatterino

// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/Emote.hpp"

#include "common/Literals.hpp"

#include <QJsonObject>

#include <unordered_map>

namespace chatterino {

using namespace literals;

bool operator==(const Emote &a, const Emote &b)
{
    return std::tie(a.homePage, a.name, a.tooltip, a.images, a.modifierFlags,
                    a.modifierPlacement, a.modifierSource) ==
           std::tie(b.homePage, b.name, b.tooltip, b.images, b.modifierFlags,
                    b.modifierPlacement, b.modifierSource);
}

bool operator!=(const Emote &a, const Emote &b)
{
    return !(a == b);
}

QJsonObject Emote::toJson() const
{
    QJsonObject obj{
        {"name"_L1, this->name.string},
        {"images"_L1, this->images.toJson()},
        {"tooltip"_L1, this->tooltip.string},
    };
    if (!this->homePage.string.isEmpty())
    {
        obj["homePage"_L1] = this->homePage.string;
    }
    if (this->zeroWidth)
    {
        obj["zeroWidth"_L1] = this->zeroWidth;
    }
    if (!this->id.string.isEmpty())
    {
        obj["id"_L1] = this->id.string;
    }
    if (!this->author.string.isEmpty())
    {
        obj["author"_L1] = this->author.string;
    }
    if (this->baseName)
    {
        obj["baseName"_L1] = this->baseName->string;
    }
    if (this->modifierPlacement != EmoteModifierPlacement::None)
    {
        obj["modifierFlags"_L1] = static_cast<qint64>(this->modifierFlags);
        obj["modifierPlacement"_L1] =
            this->modifierPlacement == EmoteModifierPlacement::Prefix
                ? u"prefix"_s
                : u"suffix"_s;
        switch (this->modifierSource)
        {
            case EmoteModifierSource::BetterTTV:
                obj["modifierSource"_L1] = u"betterttv"_s;
                break;
            case EmoteModifierSource::FrankerFaceZ:
                obj["modifierSource"_L1] = u"frankerfacez"_s;
                break;
            case EmoteModifierSource::None:
                obj["modifierSource"_L1] = u"unknown"_s;
                break;
        }
    }

    return obj;
}

EmotePtr cachedOrMakeEmotePtr(Emote &&emote, const EmoteMap &cache)
{
    auto it = cache.find(emote.name);
    if (it != cache.end() && *it->second == emote)
    {
        return it->second;
    }

    return std::make_shared<Emote>(std::move(emote));
}

EmotePtr cachedOrMakeEmotePtr(
    Emote &&emote,
    std::unordered_map<EmoteId, std::weak_ptr<const Emote>> &cache,
    std::mutex &mutex, const EmoteId &id)
{
    std::lock_guard<std::mutex> guard(mutex);

    auto shared = cache[id].lock();
    if (shared && *shared == emote)
    {
        return shared;
    }
    else
    {
        shared = std::make_shared<Emote>(std::move(emote));
        cache[id] = shared;
        return shared;
    }
}

EmoteMap::const_iterator EmoteMap::findEmote(const QString &emoteNameHint,
                                             const QString &emoteID) const
{
    auto it = this->end();
    if (!emoteNameHint.isEmpty())
    {
        it = this->find(EmoteName{emoteNameHint});
    }

    if (it == this->end() || it->second->id.string != emoteID)
    {
        it = std::find_if(this->begin(), this->end(),
                          [emoteID](const auto entry) {
                              return entry.second->id.string == emoteID;
                          });
    }
    return it;
}

}  // namespace chatterino

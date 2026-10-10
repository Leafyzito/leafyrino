// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"
#include "messages/ImageSet.hpp"

#include <QStringList>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

class QJsonObject;

namespace chatterino {

enum class EmoteModifierPlacement : uint8_t {
    None,
    Prefix,
    Suffix,
};

enum class EmoteModifierSource : uint8_t {
    None,
    BetterTTV,
    FrankerFaceZ,
};

namespace emote_modifiers {

constexpr uint32_t HIDDEN = 1U;
constexpr uint32_t FLIP_X = 1U << 1U;
constexpr uint32_t FLIP_Y = 1U << 2U;
constexpr uint32_t WIDE = 1U << 3U;
constexpr uint32_t SLIDE = 1U << 4U;
constexpr uint32_t APPEAR = 1U << 5U;
constexpr uint32_t LEAVE = 1U << 6U;
constexpr uint32_t SPIN = 1U << 7U;
constexpr uint32_t ROTATE_90 = 1U << 8U;
constexpr uint32_t RAINBOW = 1U << 11U;
constexpr uint32_t HYPER_RED = 1U << 12U;
constexpr uint32_t SHAKE = 1U << 13U;
constexpr uint32_t CURSED = 1U << 14U;
constexpr uint32_t JAM = 1U << 15U;
constexpr uint32_t BOUNCE = 1U << 16U;
constexpr uint32_t ZERO_SPACE = 1U << 17U;

constexpr uint32_t ROTATE_LEFT = 1U << 24U;
constexpr uint32_t ROTATE_RIGHT = 1U << 25U;
constexpr uint32_t PARTY = 1U << 26U;
constexpr uint32_t BTTV_SHAKE = 1U << 27U;
constexpr uint32_t BTTV_WIDE = 1U << 28U;

constexpr uint32_t SUPPORTED =
    FLIP_X | FLIP_Y | WIDE | SLIDE | APPEAR | LEAVE | SPIN | ROTATE_90 |
    RAINBOW | HYPER_RED | SHAKE | CURSED | JAM | BOUNCE | ZERO_SPACE |
    ROTATE_LEFT | ROTATE_RIGHT | PARTY | BTTV_SHAKE | BTTV_WIDE;

constexpr uint32_t EFFECTS = SUPPORTED & ~HIDDEN;

constexpr uint32_t ANIMATED = SLIDE | APPEAR | LEAVE | SPIN | RAINBOW |
                              HYPER_RED | SHAKE | JAM | BOUNCE | PARTY |
                              BTTV_SHAKE;

}  // namespace emote_modifiers

struct Emote {
    EmoteName name;
    ImageSet images;
    Tooltip tooltip;
    Url homePage;
    bool zeroWidth{};
    EmoteId id;
    EmoteAuthor author;
    /**
     * If this emote is aliased, this contains
     * the original (base) name of the emote.
     */
    std::optional<EmoteName> baseName;
    QStringList tags;

    uint32_t modifierFlags{};
    EmoteModifierPlacement modifierPlacement = EmoteModifierPlacement::None;
    EmoteModifierSource modifierSource = EmoteModifierSource::None;

    const QString &getCopyString() const
    {
        return this->name.string;
    }

    QJsonObject toJson() const;
};

bool operator==(const Emote &a, const Emote &b);
bool operator!=(const Emote &a, const Emote &b);

using EmotePtr = std::shared_ptr<const Emote>;

class EmoteMap : public std::unordered_map<EmoteName, EmotePtr, EmoteNameHash,
                                           std::equal_to<>>
{
public:
    EmoteMap::const_iterator findEmote(const QString &emoteNameHint,
                                       const QString &emoteID) const;
};

inline const std::shared_ptr<const EmoteMap> EMPTY_EMOTE_MAP =
    std::make_shared<const EmoteMap>();

EmotePtr cachedOrMakeEmotePtr(Emote &&emote, const EmoteMap &cache);
EmotePtr cachedOrMakeEmotePtr(
    Emote &&emote,
    std::unordered_map<EmoteId, std::weak_ptr<const Emote>> &cache,
    std::mutex &mutex, const EmoteId &id);

}  // namespace chatterino

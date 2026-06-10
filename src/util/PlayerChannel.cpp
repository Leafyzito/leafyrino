// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/PlayerChannel.hpp"

#include "common/Common.hpp"
#include "providers/kick/KickApi.hpp"
#include "util/QStringHash.hpp"

#include <unordered_map>

namespace chatterino {

namespace {

std::unordered_map<QString, std::weak_ptr<PlayerChannel>> &playerChannels()
{
    static std::unordered_map<QString, std::weak_ptr<PlayerChannel>> map;
    return map;
}

}  // namespace

PlayerChannel::PlayerChannel(const QString &name, Platform platform)
    : Channel(name.toLower(), Type::Player)
    , platform_(platform)
    , displayName_(name)
    , playerUrl_(buildPlayerUrl(this->getName(), platform))
{
    const auto *platformLabel =
        platform == Platform::Kick ? u"Kick" : u"Twitch";
    if (this->displayName_.isEmpty())
    {
        this->localizedName_ = QString(platformLabel) % u" Player";
    }
    else
    {
        this->localizedName_ =
            this->displayName_ % u' ' % platformLabel % u" Player";
    }
}

ChannelPtr PlayerChannel::getOrCreate(const QString &name, Platform platform)
{
    const auto trimmed = name.trimmed();
    if (trimmed.isEmpty())
    {
        return std::make_shared<PlayerChannel>(QString{}, platform);
    }

    const auto key = cacheKey(trimmed, platform);
    auto &map = playerChannels();
    if (auto existing = map[key].lock())
    {
        return existing;
    }

    auto channel = std::make_shared<PlayerChannel>(trimmed, platform);
    map[key] = channel;
    return channel;
}

std::optional<PlayerChannel::Platform> PlayerChannel::platformFromSlug(
    QStringView slug)
{
    if (slug == u"twitch")
    {
        return Platform::Twitch;
    }
    if (slug == u"kick")
    {
        return Platform::Kick;
    }
    return std::nullopt;
}

QStringView PlayerChannel::platformSlug(Platform platform)
{
    switch (platform)
    {
        case Platform::Twitch:
            return u"twitch";
        case Platform::Kick:
            return u"kick";
    }
    return u"twitch";
}

PlayerChannel::Platform PlayerChannel::platform() const
{
    return this->platform_;
}

const QString &PlayerChannel::playerUrl() const
{
    return this->playerUrl_;
}

bool PlayerChannel::canSendMessage() const
{
    return false;
}

bool PlayerChannel::isWritable() const
{
    return false;
}

bool PlayerChannel::isEmpty() const
{
    return this->getName().isEmpty();
}

const QString &PlayerChannel::getDisplayName() const
{
    return this->displayName_;
}

const QString &PlayerChannel::getLocalizedName() const
{
    return this->localizedName_;
}

QString PlayerChannel::cacheKey(const QString &name, Platform platform)
{
    return QString(platformSlug(platform)) % u':' % name.toLower();
}

QString PlayerChannel::buildPlayerUrl(const QString &name, Platform platform)
{
    switch (platform)
    {
        case Platform::Twitch:
            return QString(TWITCH_PLAYER_URL).arg(name.toLower());
        case Platform::Kick:
            return QString(KICK_PLAYER_URL).arg(KickApi::slugify(name));
    }
    return {};
}

}  // namespace chatterino

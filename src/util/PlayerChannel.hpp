// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"

#include <optional>

namespace chatterino {

class PlayerChannel : public Channel
{
public:
    enum class Platform : uint8_t {
        Twitch,
        Kick,
    };

    explicit PlayerChannel(const QString &name,
                           Platform platform = Platform::Twitch);

    static ChannelPtr getOrCreate(const QString &name,
                                  Platform platform = Platform::Twitch);

    static std::optional<Platform> platformFromSlug(QStringView slug);
    static QStringView platformSlug(Platform platform);

    Platform platform() const;
    const QString &playerUrl() const;

    bool canSendMessage() const override;
    bool isWritable() const override;
    bool isEmpty() const override;
    const QString &getDisplayName() const override;
    const QString &getLocalizedName() const override;

private:
    static QString cacheKey(const QString &name, Platform platform);
    static QString buildPlayerUrl(const QString &name, Platform platform);

    Platform platform_;
    QString displayName_;
    QString localizedName_;
    QString playerUrl_;
};

}  // namespace chatterino

Q_DECLARE_METATYPE(chatterino::PlayerChannel::Platform)

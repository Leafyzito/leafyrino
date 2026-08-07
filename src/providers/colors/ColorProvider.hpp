// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/twitch/api/HelixEnums.hpp"

#include <QColor>

#include <memory>
#include <unordered_map>
#include <vector>

namespace chatterino {

enum class ColorType {
    SelfHighlight,
    Subscription,
    Follow,
    Whisper,
    RedeemedHighlight,
    WatchStreak,
    FirstMessageHighlight,
    ThreadMessageHighlight,

    // Used in automatic highlights of your own messages
    SelfMessageHighlight,
    AutomodHighlight,
    AnnouncementHighlight,
    AnnouncementBlue,
    AnnouncementGreen,
    AnnouncementOrange,
    AnnouncementPurple,
};

ColorType colorTypeFromHelixAnnouncementColor(
    HelixAnnouncementColor announcementColor, bool enableColoredAnnouncements);

class ColorProvider
{
public:
    static const ColorProvider &instance();

    std::shared_ptr<QColor> color(ColorType type) const;

    QSet<QColor> recentColors() const;

    const std::vector<QColor> &defaultColors() const;

private:
    ColorProvider();

    void initTypeColorMap();
    void initDefaultColors();

    std::unordered_map<ColorType, std::shared_ptr<QColor>> typeColorMap_;
    std::vector<QColor> defaultColors_;
};
}  // namespace chatterino

inline uint qHash(const QColor &key)
{
    return qHash(key.name(QColor::HexArgb));
}

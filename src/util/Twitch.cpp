// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/Twitch.hpp"

#include "util/QStringHash.hpp"

#include <QDesktopServices>
#include <QUrl>

#include <unordered_map>

namespace chatterino {

namespace {

const auto TWITCH_USER_LOGIN_PATTERN = R"(^[a-z0-9]\w{0,24}$)";

const std::unordered_map<QString, QString> HELIX_COLOR_REPLACEMENTS{
    {"blueviolet", "blue_violet"},   {"cadetblue", "cadet_blue"},
    {"dodgerblue", "dodger_blue"},   {"goldenrod", "golden_rod"},
    {"hotpink", "hot_pink"},         {"orangered", "orange_red"},
    {"seagreen", "sea_green"},       {"springgreen", "spring_green"},
    {"yellowgreen", "yellow_green"},
};

const std::unordered_map<QString, QString> HELIX_COLOR_DISPLAY_HEX{
    {"blue", "#0000FF"},         {"blue_violet", "#8A2BE2"},
    {"cadet_blue", "#5F9EA0"},   {"chocolate", "#D2691E"},
    {"coral", "#FF7F50"},        {"dodger_blue", "#1E90FF"},
    {"firebrick", "#B22222"},    {"golden_rod", "#DAA520"},
    {"green", "#008000"},        {"hot_pink", "#FF69B4"},
    {"orange_red", "#FF4500"},   {"red", "#FF0000"},
    {"sea_green", "#2E8B57"},    {"spring_green", "#00FF7F"},
    {"yellow_green", "#9ACD32"},
};

}  // namespace

extern const QStringList VALID_HELIX_COLORS{
    "blue",        "blue_violet", "cadet_blue", "chocolate",    "coral",
    "dodger_blue", "firebrick",   "golden_rod", "green",        "hot_pink",
    "orange_red",  "red",         "sea_green",  "spring_green", "yellow_green",
};

void openTwitchUsercard(QString channel, QString username)
{
    QDesktopServices::openUrl("https://www.twitch.tv/popout/" + channel +
                              "/viewercard/" + username);
}

void stripUserName(QString &userName)
{
    if (userName.startsWith('@'))
    {
        userName.remove(0, 1);
    }
    if (userName.endsWith(','))
    {
        userName.chop(1);
    }
}

void stripChannelName(QString &channelName)
{
    if (channelName.startsWith('@') || channelName.startsWith('#'))
    {
        channelName.remove(0, 1);
    }
    if (channelName.endsWith(','))
    {
        channelName.chop(1);
    }
}

QString cleanChannelName(const QString &dirtyChannelName)
{
    if (dirtyChannelName.startsWith('#'))
    {
        return dirtyChannelName.mid(1).toLower();
    }

    return dirtyChannelName.toLower();
}

std::pair<ParsedUserName, ParsedUserID> parseUserNameOrID(const QString &input)
{
    if (input.startsWith("id:"))
    {
        return {
            {},
            input.mid(3),
        };
    }

    QString userName = input;

    if (userName.startsWith('@') || userName.startsWith('#'))
    {
        userName.remove(0, 1);
    }
    if (userName.endsWith(','))
    {
        userName.chop(1);
    }

    return {
        userName,
        {},
    };
}

QRegularExpression twitchUserNameRegexp()
{
    static QRegularExpression re(
        TWITCH_USER_LOGIN_PATTERN,
        QRegularExpression::PatternOption::CaseInsensitiveOption);

    return re;
}

QRegularExpression twitchUserLoginRegexp()
{
    static QRegularExpression re(TWITCH_USER_LOGIN_PATTERN);

    return re;
}

void cleanHelixColorName(QString &color)
{
    color = color.toLower();
    auto it = HELIX_COLOR_REPLACEMENTS.find(color);

    if (it == HELIX_COLOR_REPLACEMENTS.end())
    {
        return;
    }

    color = it->second;
}

QString helixColorDisplayHex(const QString &helixColorName)
{
    auto it = HELIX_COLOR_DISPLAY_HEX.find(helixColorName);
    if (it == HELIX_COLOR_DISPLAY_HEX.end())
    {
        return {};
    }

    return it->second;
}

QString formatHelixColorLabel(const QString &helixColorName)
{
    auto parts = helixColorName.split('_');
    QStringList formatted;
    formatted.reserve(parts.size());

    for (const auto &part : parts)
    {
        if (part.isEmpty())
        {
            continue;
        }

        auto word = part;
        word[0] = word[0].toUpper();
        formatted.append(word);
    }

    return formatted.join(' ');
}

std::optional<QString> helixColorNameFromDisplayHex(const QString &hex)
{
    const auto normalized = hex.trimmed().toUpper();
    if (normalized.isEmpty())
    {
        return std::nullopt;
    }

    for (const auto &[name, displayHex] : HELIX_COLOR_DISPLAY_HEX)
    {
        if (displayHex.compare(normalized, Qt::CaseInsensitive) == 0)
        {
            return name;
        }
    }

    return std::nullopt;
}

}  // namespace chatterino

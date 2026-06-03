// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/completion/sources/CommandSource.hpp"

#include "Application.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/completion/sources/Helpers.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "singletons/Settings.hpp"
#include "widgets/splits/InputCompletionItem.hpp"

#include <QHash>
#include <QSet>

#include <algorithm>

namespace chatterino::completion {

namespace {

QString commandUsage(const QString &command)
{
    static const QHash<QString, QString> usages{
        {"/announce", "<message>"},
        {"/announceblue", "<message>"},
        {"/announcegreen", "<message>"},
        {"/announceorange", "<message>"},
        {"/announcepurple", "<message>"},
        {"/ban", "<username> [reason]"},
        {"/banid", "<user-id> [reason]"},
        {"/block", "<username>"},
        {"/blockterm", "<term>"},
        {"/bot", "[message]"},
        {"/cancelpoll", ""},
        {"/cancelprediction", ""},
        {"/chatters", ""},
        {"/clear", ""},
        {"/clearmessages", ""},
        {"/clip", ""},
        {"/color", "<color>"},
        {"/commercial", "<length>"},
        {"/completeprediction", "<outcome>"},
        {"/copy", "<text>"},
        {"/debug-args", ""},
        {"/debug-env", ""},
        {"/debug-eventsub", ""},
        {"/debug-force-image-gc", ""},
        {"/debug-force-image-unload", ""},
        {"/debug-force-layout-channel-views", ""},
        {"/debug-increment-image-generation", ""},
        {"/debug-invalidate-buffers", ""},
        {"/debug-kick-raw-event", ""},
        {"/debug-test", ""},
        {"/debug-update-to-no-stream", ""},
        {"/delete", "<message-id>"},
        {"/disconnect", ""},
        {"/editor", "<username>"},
        {"/emoteonly", ""},
        {"/emoteonlyoff", ""},
        {"/endpoll", ""},
        {"/fakemsg", "<message>"},
        {"/follow", "[username]"},
        {"/followers", "[duration]"},
        {"/followersoff", ""},
        {"/founders", ""},
        {"/help", ""},
        {"/host", "<username>"},
        {"/ignore", "<username>"},
        {"/leadmod", "<username>"},
        {"/lockprediction", ""},
        {"/logs", "<username> [channel]"},
        {"/lowtrust", "<username>"},
        {"/marker", "[description]"},
        {"/me", "<message>"},
        {"/mod", "<username>"},
        {"/modlogs", "[all|moderator] [range] [channel]"},
        {"/mods", ""},
        {"/monitor", "<username>"},
        {"/namehistory", "<username>"},
        {"/nuke", "<text> <timeout|ban|delete> <range>"},
        {"/openurl", "<url> [--incognito|--no-incognito]"},
        {"/pin", "<messageid/message/username>"},
        {"/poll", ""},
        {"/popout", "[channel]"},
        {"/popup", "[channel]"},
        {"/prediction", ""},
        {"/pyramid", "<height> <message>"},
        {"/raid", "<username>"},
        {"/raidcancel", ""},
        {"/raidsend", ""},
        {"/raw", "<message>"},
        {"/reply", "<username> <message>"},
        {"/requests", "[channel]"},
        {"/restrict", "<username>"},
        {"/setgame", "<game>"},
        {"/settitle", "<stream title>"},
        {"/shield", ""},
        {"/shieldoff", ""},
        {"/shoutout", "<username>"},
        {"/slow", "[duration]"},
        {"/slowoff", ""},
        {"/spam", "<count> <message>"},
        {"/streamlink", "[channel]"},
        {"/subscribers", ""},
        {"/subscribersoff", ""},
        {"/saytranslate", "<language> <message>"},
        {"/test-chatters", ""},
        {"/timeout", "<username> [duration] [reason]"},
        {"/tl", "<language> <message>"},
        {"/translate", "<message>"},
        {"/translateto", "<language> <message>"},
        {"/unban", "<username>"},
        {"/unblock", "<username>"},
        {"/unblockterm", "<term>"},
        {"/uneditor", "<username>"},
        {"/unfollow", "[username]"},
        {"/unhost", ""},
        {"/unignore", "<username>"},
        {"/unleadmod", "<username>"},
        {"/unmod", "<username>"},
        {"/unmonitor", "<username>"},
        {"/unpin", ""},
        {"/unraid", ""},
        {"/unrestrict", "<username>"},
        {"/untimeout", "<username>"},
        {"/unvip", "<username>"},
        {"/uniquechat", ""},
        {"/uniquechatoff", ""},
        {"/unstable-set-user-color", "<username> <color>"},
        {"/user", "<username> [channel]"},
        {"/usercard", "<username> [channel]"},
        {"/vip", "<username>"},
        {"/vips", ""},
        {"/warn", "<username> <reason>"},
        {".w", "<username> <message>"},
        {"/w", "<username> <message>"},
        {"/whisper", "<username> <message>"},
    };

    return usages.value(command.toLower());
}

void addCommand(const QString &command, std::vector<CommandItem> &out)
{
    const auto normalized = command.startsWith('/') || command.startsWith('.')
                                ? command
                                : QStringLiteral("/") + command;
    const auto usage = commandUsage(normalized);

    if (command.startsWith('/') || command.startsWith('.'))
    {
        out.push_back({
            .name = command.mid(1),
            .prefix = command.at(0),
            .usage = usage,
        });
    }
    else
    {
        out.push_back({
            .name = command,
            .prefix = "",
            .usage = usage,
        });
    }
}

const QSet<QString> &currentAccountModCommands()
{
    static const QSet<QString> commands{
        "/announce",
        "/announceblue",
        "/announcegreen",
        "/announceorange",
        "/announcepurple",
        "/ban",
        "/banid",
        "/blockterm",
        "/bot",
        "/cancelpoll",
        "/cancelprediction",
        "/chatters",
        "/clear",
        "/clearmessages",
        "/commercial",
        "/completeprediction",
        "/delete",
        "/emoteonly",
        "/emoteonlyoff",
        "/endpoll",
        "/followers",
        "/followersoff",
        "/host",
        "/marker",
        "/lockprediction",
        "/lowtrust",
        "/mod",
        "/monitor",
        "/nuke",
        "/r9kbeta",
        "/r9kbetaoff",
        "/raid",
        "/requests",
        "/restrict",
        "/setgame",
        "/settitle",
        "/shield",
        "/shieldoff",
        "/shoutout",
        "/slow",
        "/slowoff",
        "/subscribers",
        "/subscribersoff",
        "/timeout",
        "/unban",
        "/unhost",
        "/unblockterm",
        "/unmod",
        "/unmonitor",
        "/unpin",
        "/unraid",
        "/unrestrict",
        "/untimeout",
        "/unvip",
        "/vip",
        "/warn",
    };
    return commands;
}

const QSet<QString> &moltorinoModerationCommands()
{
    static const QSet<QString> commands{
        "/blockterm",        "/cancelpoll",
        "/cancelprediction", "/completeprediction",
        "/endpoll",          "/lockprediction",
        "/modlogs",          "/pin",
        "/unblockterm",      "/unpin",
    };
    return commands;
}

const QSet<QString> &currentAccountBroadcasterCommands()
{
    static const QSet<QString> commands{
        "/raid",
        "/raidcancel",
        "/raidsend",
        "/unraid",
    };
    return commands;
}

const QSet<QString> &roleManagementCommands()
{
    static const QSet<QString> commands{
        "/editor",
        "/leadmod",
        "/uneditor",
        "/unleadmod",
    };
    return commands;
}

bool hasMoltorinoModerationAccess(const Channel *channel)
{
    auto *twitchChannel = dynamic_cast<const TwitchChannel *>(channel);
    if (twitchChannel == nullptr)
    {
        return false;
    }

    QString ignored;
    return MoltorinoAuth::resolveModerationToken(
               twitchChannel->roomId(), twitchChannel->getName(), &ignored)
        .hasToken();
}

bool hasMoltorinoBroadcasterAccess(const Channel *channel)
{
    auto *twitchChannel = dynamic_cast<const TwitchChannel *>(channel);
    if (twitchChannel == nullptr)
    {
        return false;
    }

    QString ignored;
    return MoltorinoAuth::resolveBroadcasterToken(
               twitchChannel->roomId(), twitchChannel->getName(), &ignored)
        .hasToken();
}

bool hasMoltorinoRoleManagementAccess(const Channel *channel)
{
    auto *twitchChannel = dynamic_cast<const TwitchChannel *>(channel);
    if (twitchChannel == nullptr)
    {
        return false;
    }

    QString ignored;
    return MoltorinoAuth::resolveSavedBroadcasterToken(
               twitchChannel->roomId(), twitchChannel->getName(), &ignored)
        .hasToken();
}

bool hasBotBadgeAuth()
{
    const auto &settings = *getSettings();
    return !settings.botBadgeAppAccessToken.getValue().trimmed().isEmpty() &&
           !settings.botBadgeClientID.getValue().trimmed().isEmpty() &&
           !settings.botBadgeUserID.getValue().trimmed().isEmpty();
}

QString normalizedCommand(const CommandItem &item)
{
    const auto prefix =
        item.prefix.isEmpty() ? QStringLiteral("/") : item.prefix;
    return (prefix + item.name).toLower();
}

bool isInternalCommand(const QString &command)
{
    return command.startsWith(QStringLiteral("/debug-")) ||
           command.startsWith(QStringLiteral("/c2-")) ||
           command.startsWith(QStringLiteral("/unstable-")) ||
           command == QStringLiteral("/fakemsg") ||
           command == QStringLiteral("/test-chatters");
}

bool moltorinoFeatureHandlesCommand(const QString &command)
{
    const auto &settings = *getSettings();
    if (command == "/cancelpoll" || command == "/endpoll")
    {
        return settings.enablePolls;
    }
    if (command == "/cancelprediction" || command == "/completeprediction" ||
        command == "/lockprediction")
    {
        return settings.enablePredictions;
    }
    if (command == "/modlogs" || command == "/pin" || command == "/unpin")
    {
        return true;
    }
    if (command == "/blockterm" || command == "/unblockterm")
    {
        return true;
    }
    if (currentAccountBroadcasterCommands().contains(command))
    {
        return true;
    }
    return false;
}

bool needsMoltorinoModerationAccess(const std::vector<CommandItem> &items)
{
    return std::any_of(items.begin(), items.end(), [](const auto &item) {
        const auto command = normalizedCommand(item);
        return moltorinoFeatureHandlesCommand(command) &&
               (moltorinoModerationCommands().contains(command) ||
                currentAccountBroadcasterCommands().contains(command));
    });
}

bool needsMoltorinoBroadcasterAccess(const std::vector<CommandItem> &items)
{
    return std::any_of(items.begin(), items.end(), [](const auto &item) {
        const auto command = normalizedCommand(item);
        return moltorinoFeatureHandlesCommand(command) &&
               currentAccountBroadcasterCommands().contains(command);
    });
}

bool needsMoltorinoRoleManagementAccess(const std::vector<CommandItem> &items)
{
    return std::any_of(items.begin(), items.end(), [](const auto &item) {
        return roleManagementCommands().contains(normalizedCommand(item));
    });
}

bool shouldHideCommand(const CommandItem &item, bool hideUnavailable,
                       bool hasCurrentAccountModRights,
                       bool hasCurrentAccountBroadcasterRights,
                       bool hasMoltorinoModerationAccess,
                       bool hasMoltorinoBroadcasterAccess,
                       bool hasMoltorinoRoleManagementAccess,
                       bool hasBotBadgeAuth)
{
    const auto command = normalizedCommand(item);
    if (isInternalCommand(command))
    {
        return true;
    }

    if (roleManagementCommands().contains(command))
    {
        return !hasMoltorinoRoleManagementAccess;
    }

    if (command == "/bot" && !hasBotBadgeAuth)
    {
        return true;
    }

    if (!hideUnavailable)
    {
        return false;
    }

    if (command == "/modlogs")
    {
        return false;
    }

    if (currentAccountBroadcasterCommands().contains(command))
    {
        return !hasCurrentAccountBroadcasterRights &&
               !(moltorinoFeatureHandlesCommand(command) &&
                 (hasMoltorinoBroadcasterAccess || hasCurrentAccountModRights ||
                  hasMoltorinoModerationAccess));
    }

    if (hasCurrentAccountModRights)
    {
        return false;
    }

    if (moltorinoModerationCommands().contains(command) &&
        moltorinoFeatureHandlesCommand(command) && hasMoltorinoModerationAccess)
    {
        return false;
    }

    return currentAccountModCommands().contains(command) ||
           moltorinoModerationCommands().contains(command);
}

}  // namespace

CommandSource::CommandSource(std::unique_ptr<CommandStrategy> strategy,
                             ActionCallback callback, const Channel *channel)
    : strategy_(std::move(strategy))
    , callback_(std::move(callback))
    , channel_(channel)
{
    this->initializeItems();
}

void CommandSource::update(const QString &query)
{
    this->output_.clear();
    if (this->strategy_)
    {
        this->strategy_->apply(this->items_, this->output_, query);
        const bool hideUnavailable = getSettings()->hideUnavailableModCommands;
        const bool hasCurrentAccountModRights =
            this->channel_ != nullptr && this->channel_->hasModRights();
        const bool hasCurrentAccountBroadcasterRights =
            this->channel_ != nullptr && this->channel_->isBroadcaster();
        const bool moltorinoAccess =
            hideUnavailable && !hasCurrentAccountModRights &&
            needsMoltorinoModerationAccess(this->output_) &&
            hasMoltorinoModerationAccess(this->channel_);
        const bool moltorinoBroadcasterAccess =
            hideUnavailable && !hasCurrentAccountBroadcasterRights &&
            needsMoltorinoBroadcasterAccess(this->output_) &&
            hasMoltorinoBroadcasterAccess(this->channel_);
        const bool moltorinoRoleManagementAccess =
            needsMoltorinoRoleManagementAccess(this->output_) &&
            hasMoltorinoRoleManagementAccess(this->channel_);
        const bool botBadgeAuth = hasBotBadgeAuth();
        this->output_.erase(
            std::remove_if(this->output_.begin(), this->output_.end(),
                           [&](const auto &item) {
                               return shouldHideCommand(
                                   item, hideUnavailable,
                                   hasCurrentAccountModRights,
                                   hasCurrentAccountBroadcasterRights,
                                   moltorinoAccess, moltorinoBroadcasterAccess,
                                   moltorinoRoleManagementAccess, botBadgeAuth);
                           }),
            this->output_.end());
    }
}

void CommandSource::addToListModel(GenericListModel &model,
                                   size_t maxCount) const
{
    addVecToListModel(this->output_, model, maxCount,
                      [this](const CommandItem &command) {
                          return std::make_unique<InputCompletionItem>(
                              nullptr, command.name, this->callback_);
                      });
}

void CommandSource::addToStringList(QStringList &list, size_t maxCount,
                                    bool /* isFirstWord */) const
{
    addVecToStringList(this->output_, list, maxCount,
                       [](const CommandItem &command) {
                           return command.prefix + command.name + " ";
                       });
}

void CommandSource::initializeItems()
{
    std::vector<CommandItem> commands;

#ifdef CHATTERINO_HAVE_PLUGINS
    for (const auto &command : getApp()->getCommands()->pluginCommands())
    {
        addCommand(command, commands);
    }
#endif

    // Custom Chatterino commands
    for (const auto &command : getApp()->getCommands()->items)
    {
        addCommand(command.name, commands);
    }

    // Default Chatterino commands
    auto x = getApp()->getCommands()->getDefaultChatterinoCommandList();
    for (const auto &command : x)
    {
        addCommand(command, commands);
    }

    // Default Twitch commands
    for (const auto &command : TWITCH_DEFAULT_COMMANDS)
    {
        addCommand(command, commands);
    }

    this->items_ = std::move(commands);
}

const std::vector<CommandItem> &CommandSource::output() const
{
    return this->output_;
}

}  // namespace chatterino::completion

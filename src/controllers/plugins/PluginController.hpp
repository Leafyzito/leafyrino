// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#ifdef CHATTERINO_HAVE_PLUGINS

#    include "common/websockets/WebSocketPool.hpp"
#    include "controllers/commands/CommandContext.hpp"
#    include "controllers/plugins/Plugin.hpp"

#    include <boost/signals2/signal.hpp>
#    include <QDir>
#    include <QFileInfo>
#    include <QJsonArray>
#    include <QJsonObject>
#    include <QString>
#    include <sol/forward.hpp>

#    include <map>
#    include <memory>
#    include <utility>

struct lua_State;

namespace chatterino {

class Settings;
class Paths;

class PluginController
{
    const Paths &paths;

public:
    explicit PluginController(const Paths &paths_);

    void initialize(Settings &settings);

    QString tryExecPluginCommand(const QString &commandName,
                                 const CommandContext &ctx);

    Plugin *getPluginByStatePtr(lua_State *L);

    const std::map<QString, std::unique_ptr<Plugin>> &plugins() const;

    bool reload(const QString &id);

    static bool isPluginEnabled(const QString &id);

    std::pair<bool, QStringList> updateCustomCompletions(
        const QString &query, const QString &fullTextContent,
        int cursorPosition, bool isFirstWord) const;

    WebSocketPool &webSocketPool();

    pajlada::Signals::Signal<Plugin *> onPluginLoaded;
    pajlada::Signals::NoArgSignal onPluginsUpdated;

private:
    void loadPlugins();
    void load(const QFileInfo &index, const QDir &pluginDir,
              const PluginMeta &meta);

    void openLibrariesFor(Plugin *plugin);

    void initSol(sol::state_view &lua, Plugin *plugin);

    static void loadChatterinoLib(lua_State *l);
    bool tryLoadFromDir(const QDir &pluginDir);

    void queueChangeNotification();

    std::map<QString, std::unique_ptr<Plugin>> plugins_;
    WebSocketPool webSocketPool_;

    std::vector<
        std::pair<std::string, std::function<sol::object(sol::state_view)>>>
        loaders_;

    bool changeNotificationQueued = false;

    // This is for tests, pay no attention
    friend class PluginControllerAccess;
};

}  // namespace chatterino
#endif

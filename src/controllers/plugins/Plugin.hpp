// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#ifdef CHATTERINO_HAVE_PLUGINS
#    include "controllers/plugins/api/EventType.hpp"
#    include "controllers/plugins/api/HTTPRequest.hpp"
#    include "controllers/plugins/ConnectionManager.hpp"
#    include "controllers/plugins/PluginMeta.hpp"
#    include "controllers/plugins/PluginRef.hpp"

#    include <boost/signals2/signal.hpp>
#    include <QDir>
#    include <QString>
#    include <QUrl>
#    include <semver/semver.hpp>
#    include <sol/forward.hpp>

#    include <cassert>
#    include <memory>
#    include <optional>
#    include <unordered_map>
#    include <unordered_set>
#    include <utility>
#    include <variant>
#    include <vector>

struct lua_State;
class QTimer;

namespace chatterino::lua::api {
enum class LogLevel;
}

namespace chatterino::lua {
struct SignalCallback;
}

namespace chatterino {

/// A plugin that hasn't been loaded.
///
/// Most likely, its metadata is invalid.
class UnloadedPlugin
{
public:
    QString id;
    PluginMeta meta;
    QDir loadDirectory;

    UnloadedPlugin(QString id_, PluginMeta meta_, const QDir &loadDirectory_)
        : id(std::move(id_))
        , meta(std::move(meta_))
        , loadDirectory(loadDirectory_)
    {
    }
};

/// A plugin with a valid lua state
class Plugin
{
public:
    QString id;
    PluginMeta meta;

    Plugin(QString id, lua_State *state, PluginMeta meta,
           const QDir &loadDirectory)
        : id(std::move(id))
        , meta(std::move(meta))
        , loadDirectory_(loadDirectory)
        , state_(state)
        , selfRef_(this)
    {
        // The PluginMeta here must be valid, otherwise it should be initialized
        // as an UnloadedPlugin
        assert(this->meta.isValid());
        assert(this->state_ != nullptr);
    }

    ~Plugin();

    Plugin(const Plugin &) = delete;
    Plugin(Plugin &&) = delete;
    Plugin &operator=(const Plugin &) = delete;
    Plugin &operator=(Plugin &&) = delete;

    bool registerCommand(const QString &name, sol::protected_function function);

    std::unordered_set<QString> listRegisteredCommands();

    const QDir &loadDirectory() const
    {
        return this->loadDirectory_;
    }

    QDir dataDirectory() const
    {
        return this->loadDirectory_.absoluteFilePath("data");
    }

    std::optional<sol::protected_function> getCompletionCallback()
    {
        if (this->state_ == nullptr || !this->error_.isNull())
        {
            return {};
        }
        auto it =
            this->callbacks.find(lua::api::EventType::CompletionRequested);
        if (it == this->callbacks.end())
        {
            return {};
        }
        return it->second;
    }

    lua::SignalCallback createCallback(sol::main_protected_function pfn);

    QString error()
    {
        return this->error_;
    }

    lua::PluginWeakRef weakRef() const;

    int addTimeout(QTimer *timer);
    void removeTimeout(QTimer *timer);

    bool hasFSPermissionFor(bool write, const QString &path);
    bool hasHTTPPermissionFor(const QUrl &url);
    bool hasNetworkPermission() const;

    void log(lua_State *L, lua::api::LogLevel level, QDebug stream,
             const sol::variadic_args &args);

    sol::state_view state();

    std::map<lua::api::EventType, sol::protected_function> callbacks;

    std::vector<std::shared_ptr<lua::api::HTTPRequest>> httpRequests;

    boost::signals2::signal<void()> onUnloaded;
    boost::signals2::signal<void(lua::api::LogLevel, const QString &)> onLog;
    lua::ConnectionManager connections;

private:
    QDir loadDirectory_;
    lua_State *state_;

    lua::PluginRef selfRef_;

    QString error_;

    std::unordered_map<QString, sol::protected_function> ownedCommands;
    std::vector<QTimer *> activeTimeouts;
    int lastTimerId = 0;

    friend class PluginController;
    friend class PluginControllerAccess;
};

using PluginPtr = std::unique_ptr<Plugin>;
using AnyPlugin = std::variant<PluginPtr, UnloadedPlugin>;

}  // namespace chatterino

#endif

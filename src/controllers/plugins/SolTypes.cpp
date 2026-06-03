// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#ifdef CHATTERINO_HAVE_PLUGINS
#    include "controllers/plugins/SolTypes.hpp"

#    include "Application.hpp"
#    include "common/QLogging.hpp"
#    include "controllers/plugins/api/Message.hpp"
#    include "controllers/plugins/LuaAPI.hpp"
#    include "controllers/plugins/PluginController.hpp"
#    include "messages/Link.hpp"

#    include <QObject>
#    include <QStringBuilder>
#    include <sol/thread.hpp>

namespace chatterino::lua {

using namespace Qt::Literals::StringLiterals;

Plugin *ThisPluginState::plugin()
{
    if (this->plugptr_ != nullptr)
    {
        return this->plugptr_;
    }
    auto *pl = getApp()->getPlugins()->getPluginByStatePtr(this->state_);
    if (pl == nullptr)
    {
        throw std::runtime_error("internal error: missing plugin");
    }
    this->plugptr_ = pl;
    return pl;
}

QString errorResultToString(const sol::protected_function_result &result)
{
    assert(!result.valid() &&
           "This function must be called on invalid/error results");

    auto optString = sol::stack::check_get<QString>(result.lua_state(), -1);
    if (optString)
    {
        return *std::move(optString);
    }

    return u"(no error message) "
           "Unless an error without a message string was explicitly thrown, "
           "this is a bug in Chatterino. Please report this."_s;
}

void logError(Plugin *plugin, QStringView context, const QString &msg)
{
    QString fullMessage = context % u" - " % msg;
    qCWarning(chatterinoLua).noquote()
        << "[" + plugin->id + ":" + plugin->meta.name + "]" << fullMessage;
    plugin->onLog(api::LogLevel::Warning, fullMessage);
}

}  // namespace chatterino::lua

bool sol_lua_check(sol::types<QString>, lua_State *L, int index,
                   chatterino::FunctionRef<sol::check_handler_type> handler,
                   sol::stack::record &tracking)
{
    return sol::stack::check<const char *>(L, index, handler, tracking);
}

QString sol_lua_get(sol::types<QString>, lua_State *L, int index,
                    sol::stack::record &tracking)
{
    auto str = sol::stack::get<std::string_view>(L, index, tracking);
    return QString::fromUtf8(str.data(), static_cast<qsizetype>(str.length()));
}

int sol_lua_push(sol::types<QString>, lua_State *L, const QString &value)
{
    return sol::stack::push(L, value.toUtf8().data());
}

bool sol_lua_check(sol::types<QStringList>, lua_State *L, int index,
                   chatterino::FunctionRef<sol::check_handler_type> handler,
                   sol::stack::record &tracking)
{
    return sol::stack::check<sol::table>(L, index, handler, tracking);
}

QStringList sol_lua_get(sol::types<QStringList>, lua_State *L, int index,
                        sol::stack::record &tracking)
{
    sol::table table = sol::stack::get<sol::table>(L, index, tracking);
    QStringList result;
    result.reserve(static_cast<qsizetype>(table.size()));
    for (size_t i = 1; i < table.size() + 1; i++)
    {
        result.append(table.get<QString>(i));
    }
    return result;
}

int sol_lua_push(sol::types<QStringList>, lua_State *L,
                 const QStringList &value)
{
    sol::table table = sol::table::create(L, static_cast<int>(value.size()));
    for (const QString &str : value)
    {
        table.add(str);
    }
    return sol::stack::push(L, table);
}

bool sol_lua_check(sol::types<QByteArray>, lua_State *L, int index,
                   chatterino::FunctionRef<sol::check_handler_type> handler,
                   sol::stack::record &tracking)
{
    return sol::stack::check<const char *>(L, index, handler, tracking);
}

QByteArray sol_lua_get(sol::types<QByteArray>, lua_State *L, int index,
                       sol::stack::record &tracking)
{
    auto str = sol::stack::get<std::string_view>(L, index, tracking);
    return {str.data(), static_cast<qsizetype>(str.length())};
}

int sol_lua_push(sol::types<QByteArray>, lua_State *L, const QByteArray &value)
{
    return sol::stack::push(L,
                            std::string_view(value.constData(), value.size()));
}

namespace chatterino::lua {

bool sol_lua_check(sol::types<chatterino::lua::ThisPluginState>, lua_State *,
                   int, chatterino::FunctionRef<sol::check_handler_type>,
                   sol::stack::record &)
{
    return true;
}

chatterino::lua::ThisPluginState sol_lua_get(
    sol::types<chatterino::lua::ThisPluginState>, lua_State *L, int,
    sol::stack::record &tracking)
{
    tracking.use(0);
    return {L};
}

int sol_lua_push(sol::types<chatterino::lua::ThisPluginState>, lua_State *L,
                 const chatterino::lua::ThisPluginState &value)
{
    return sol::stack::push(L, sol::thread(L, value));
}

}  // namespace chatterino::lua

namespace chatterino {

bool sol_lua_check(sol::types<chatterino::Link>, lua_State *L, int index,
                   chatterino::FunctionRef<sol::check_handler_type> handler,
                   sol::stack::record &tracking)
{
    return sol::stack::check<sol::table>(L, index, handler, tracking);
}

chatterino::Link sol_lua_get(sol::types<chatterino::Link>, lua_State *L,
                             int index, sol::stack::record &tracking)
{
    sol::table table = sol::stack::get<sol::table>(L, index, tracking);

    auto ty =
        table.get<sol::optional<lua::api::message::ExposedLinkType>>("type");
    if (!ty)
    {
        throw std::runtime_error("Missing 'type' in Link");
    }
    auto value = table.get<sol::optional<QString>>("value");
    if (!value)
    {
        throw std::runtime_error("Missing 'value' in Link");
    }

    return {static_cast<Link::Type>(*ty), *value};
}

int sol_lua_push(sol::types<chatterino::Link>, lua_State *L,
                 const chatterino::Link &value)
{
    sol::table table = sol::table::create(L, 0, 2);
    table.set("type",
              static_cast<lua::api::message::ExposedLinkType>(value.type));
    table.set("value", value.value);
    return sol::stack::push(L, table);
}

}  // namespace chatterino

#endif

// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#ifdef CHATTERINO_HAVE_PLUGINS

#    include <lua.h>
#    include <lualib.h>
#    include <magic_enum/magic_enum.hpp>
#    include <QList>
#    include <sol/state_view.hpp>

#    include <cassert>
#    include <string>
#    include <string_view>
#    include <type_traits>
struct lua_State;

namespace chatterino::lua {

void stackDump(lua_State *L, const QString &tag);

const QString GDB_DUMMY = "GDB_DUMMY";

QString humanErrorText(lua_State *L, int errCode);

using StackIdx = int;

StackIdx push(lua_State *L, const QString &str);
StackIdx push(lua_State *L, const std::string &str);

bool peek(lua_State *L, QString *out, StackIdx idx = -1);

QString toString(lua_State *L, StackIdx idx = -1);

class StackGuard
{
    int expected;
    lua_State *L;

public:

    StackGuard(lua_State *L)
        : expected(lua_gettop(L))
        , L(L)
    {
    }

    StackGuard(lua_State *L, int diff)
        : expected(lua_gettop(L) + diff)
        , L(L)
    {
    }

    ~StackGuard()
    {
        if (this->expected < 0)
        {
            return;
        }
        int after = lua_gettop(this->L);
        if (this->expected != after)
        {
            stackDump(this->L, "StackGuard check tripped");

            assert(false && "internal error: lua stack was not in an expected state");

        }
    }

    StackGuard operator=(StackGuard &) = delete;
    StackGuard &operator=(StackGuard &&) = delete;
    StackGuard(StackGuard &) = delete;
    StackGuard(StackGuard &&) = delete;

    void handled()
    {
        this->expected = -1;
    }
};

template <typename T, T... Additional>
    requires std::is_enum_v<T>
sol::table createEnumTable(sol::state_view &lua)
{
    constexpr auto values = magic_enum::enum_values<T>();
    auto out = lua.create_table(0, values.size() + sizeof...(Additional));
    for (const T v : values)
    {
        out.raw_set(magic_enum::enum_name<T>(v), v);
    }
    (out.raw_set(magic_enum::enum_name<Additional>(), Additional), ...);

    return out;
}

[[noreturn]]
void fail(lua_State *L, const char *fmt, auto &&...args)
{
    luaL_error(L, fmt, std::forward<decltype(args)>(args)...);

    std::terminate();
}

}

#endif

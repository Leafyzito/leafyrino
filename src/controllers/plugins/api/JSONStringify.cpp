// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/plugins/api/JSONStringify.hpp"

#ifdef CHATTERINO_HAVE_PLUGINS

#    include "controllers/plugins/LuaUtilities.hpp"

#    include <rapidjson/prettywriter.h>
#    include <rapidjson/writer.h>
#    include <sol/sol.hpp>

namespace {

using namespace chatterino::lua;

constexpr size_t ENCODE_MAX_TABLE_LENGTH = 1 << 20;
constexpr uint16_t ENCODE_MAX_DEPTH = 256;

void reserveStack(lua_State *L, int size)
{
    if (lua_checkstack(L, size) == 0)
    {
        fail(L, "Failed to reserve %d more stack slots (stack overflow)", size);
    }
}

std::optional<size_t> inferArraySize(lua_State *L)
{
    reserveStack(L, 3);

    size_t max = 0;
    bool hasNull = false;
    lua_pushnil(L);
    while (lua_next(L, -2) != 0)
    {
        if (lua_isinteger(L, -2) == 0)
        {
            lua_pop(L, 2);
            return std::nullopt;
        }

        auto key = lua_tointeger(L, -2);
        if (key <= 0)
        {
            if (key == 0 && lua_islightuserdata(L, -1) &&
                lua_touserdata(L, -1) == nullptr)
            {
                hasNull = true;
            }
            else
            {
                fail(L, "Table keys can't be negative integers");
            }
        }
        else
        {
            max = std::max(max, static_cast<size_t>(key));
        }

        lua_pop(L, 1);
    }

    if (max > ENCODE_MAX_TABLE_LENGTH)
    {
        fail(L, "Table is too big");
    }

    if (max == 0 && !hasNull)
    {
        return std::nullopt;
    }

    return max;
}

void stringifyValue(lua_State *L, auto &writer, uint16_t depth);

void stringifyArray(lua_State *L, auto &writer, uint16_t depth, size_t length)
{
    reserveStack(L, 2);

    if (!writer.StartArray())
    {
        fail(L, "Failed to write array start");
    }

    for (size_t i = 0; i < length; i++)
    {
        lua_rawgeti(L, -1, static_cast<lua_Integer>(i) + 1);
        stringifyValue(L, writer, depth);
        lua_pop(L, 1);
    }

    if (!writer.EndArray())
    {
        fail(L, "Failed to write array end");
    }
}

void stringifyObject(lua_State *L, auto &writer, uint16_t depth)
{
    reserveStack(L, 3);

    if (!writer.StartObject())
    {
        fail(L, "Failed to write object start");
    }

    size_t items = 0;
    lua_pushnil(L);
    while (lua_next(L, -2) != 0)
    {
        items++;
        if (items > ENCODE_MAX_TABLE_LENGTH)
        {
            fail(L, "Too many items in table");
        }

        auto keyType = lua_type(L, -2);
        if (keyType != LUA_TSTRING)
        {
            fail(L, "Object key was not a string, got %s",
                 lua_typename(L, keyType));
        }
        size_t len = 0;
        const char *str = lua_tolstring(L, -2, &len);

        if (!writer.Key(str, len, true))
        {
            fail(L, "Failed to write object key");
        }

        stringifyValue(L, writer, depth);
        lua_pop(L, 1);
    }

    if (!writer.EndObject())
    {
        fail(L, "Failed to write object end");
    }
}

void stringifyValue(lua_State *L, auto &writer, uint16_t depth)
{
    auto typeId = lua_type(L, -1);
    bool ok = true;
    switch (typeId)
    {
        case LUA_TNIL:
            ok = writer.Null();
            break;
        case LUA_TBOOLEAN:
            ok = writer.Bool(lua_toboolean(L, -1) != 0);
            break;
        case LUA_TNUMBER: {
            if (lua_isinteger(L, -1))
            {
                ok = writer.Int64(lua_tointeger(L, -1));
            }
            else
            {
                ok = writer.Double(lua_tonumber(L, -1));
            }
        }
        break;
        case LUA_TSTRING: {
            size_t len = 0;
            const char *str = lua_tolstring(L, -1, &len);
            ok = writer.String(str, len, true);
        }
        break;
        case LUA_TTABLE: {
            if (depth >= ENCODE_MAX_DEPTH)
            {
                fail(L, "Too deep");
            }

            auto arraySize = inferArraySize(L);
            if (arraySize)
            {
                stringifyArray(L, writer, depth + 1, *arraySize);
            }
            else
            {
                stringifyObject(L, writer, depth + 1);
            }
        }
        break;
        case LUA_TLIGHTUSERDATA: {
            auto *ptr = lua_touserdata(L, -1);
            if (ptr == nullptr)
            {
                ok = writer.Null();
            }
            else
            {
                fail(L, "Unsupported type: lightuserdata");
            }
        }
        break;
        case LUA_TFUNCTION:
        case LUA_TUSERDATA:
        case LUA_TTHREAD:
        default:
            fail(L, "Unsupported type: %s", lua_typename(L, typeId));
    }

    if (!ok)
    {
        fail(L, "Failed to format %s", lua_typename(L, typeId));
    }
}

}  // namespace

namespace chatterino::lua::api {

int jsonStringify(lua_State *L)
{
    auto nArgs = lua_gettop(L);
    luaL_argcheck(L, nArgs >= 1, 1, "expected at at least one argument");

    bool pretty = false;
    char indentChar = ' ';
    unsigned int indentSize = 4;
    if (nArgs >= 2)
    {
        auto tbl = sol::stack::check_get<sol::table>(L);
        if (tbl)
        {
            pretty = tbl->get_or("pretty", pretty);
            indentChar = tbl->get_or<char>("indent_char", indentChar);
            indentSize = tbl->get_or("indent_size", indentSize);
        }

        lua_pop(L, nArgs - 1);
    }

    rapidjson::StringBuffer sb;
    if (pretty)
    {
        rapidjson::PrettyWriter pw(
            sb, static_cast<rapidjson::CrtAllocator *>(nullptr), 32);
        pw.SetIndent(indentChar, indentSize);
        stringifyValue(L, pw, 0);
    }
    else
    {
        rapidjson::Writer w(sb);
        stringifyValue(L, w, 0);
    }

    lua_pushlstring(L, sb.GetString(), sb.GetSize());
    return 1;
}

}  // namespace chatterino::lua::api

#endif

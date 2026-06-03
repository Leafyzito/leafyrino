// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#ifdef CHATTERINO_HAVE_PLUGINS
#    include "controllers/plugins/api/ChannelRef.hpp"
#    include "controllers/plugins/Plugin.hpp"
#    include "controllers/plugins/SolTypes.hpp"

#    include <lua.h>
#    include <QList>
#    include <QString>
#    include <sol/table.hpp>

#    include <cassert>
#    include <memory>

struct lua_State;
namespace chatterino::lua::api {

enum class LogLevel { Debug, Info, Warning, Critical };

struct CompletionList {
    CompletionList(const sol::table &);

    QStringList values;

    bool hideOthers{};
};

struct CompletionEvent {
    QString query;

    QString full_text_content;

    int cursor_position{};

    bool is_first_word{};
};

sol::table toTable(lua_State *L, const CompletionEvent &ev);

void c2_register_callback(ThisPluginState L, EventType evtType,
                          sol::protected_function callback);

void c2_log(ThisPluginState L, LogLevel lvl, sol::variadic_args args);

void c2_later(ThisPluginState L, sol::protected_function callback, int time);

sol::variadic_results g_load(ThisPluginState s, sol::object data);
void g_print(ThisPluginState L, sol::variadic_args args);

void package_loadlib(sol::variadic_args args);

int searcherAbsolute(lua_State *L);
int searcherRelative(lua_State *L);

struct UserData {
    enum class Type {
        Channel,
        HTTPRequest,
        HTTPResponse,
    };
    Type type;
    bool isWeak;
};

template <UserData::Type T, typename U>
struct WeakPtrUserData : public UserData {
    std::weak_ptr<U> target;

    WeakPtrUserData(std::weak_ptr<U> t)
        : UserData()
        , target(t)
    {
        this->type = T;
        this->isWeak = true;
    }

    static WeakPtrUserData<T, U> *create(lua_State *L, std::weak_ptr<U> target)
    {
        void *ptr = lua_newuserdata(L, sizeof(WeakPtrUserData<T, U>));
        return new (ptr) WeakPtrUserData<T, U>(target);
    }

    static WeakPtrUserData<T, U> *from(UserData *target)
    {
        if (!target->isWeak)
        {
            return nullptr;
        }
        if (target->type != T)
        {
            return nullptr;
        }
        return reinterpret_cast<WeakPtrUserData<T, U> *>(target);
    }

    static WeakPtrUserData<T, U> *from(void *target)
    {
        return from(reinterpret_cast<UserData *>(target));
    }

    static int destroy(lua_State *L)
    {
        auto self = WeakPtrUserData<T, U>::from(lua_touserdata(L, -1));

        assert(self->isWeak);

        self->target.reset();
        lua_pop(L, 1);
        return 0;
    }
};

template <UserData::Type T, typename U>
struct SharedPtrUserData : public UserData {
    std::shared_ptr<U> target;

    SharedPtrUserData(std::shared_ptr<U> t)
        : UserData()
        , target(t)
    {
        this->type = T;
        this->isWeak = false;
    }

    static SharedPtrUserData<T, U> *create(lua_State *L,
                                           std::shared_ptr<U> target)
    {
        void *ptr = lua_newuserdata(L, sizeof(SharedPtrUserData<T, U>));
        return new (ptr) SharedPtrUserData<T, U>(target);
    }

    static SharedPtrUserData<T, U> *from(UserData *target)
    {
        if (target->isWeak)
        {
            return nullptr;
        }
        if (target->type != T)
        {
            return nullptr;
        }
        return reinterpret_cast<SharedPtrUserData<T, U> *>(target);
    }

    static SharedPtrUserData<T, U> *from(void *target)
    {
        return from(reinterpret_cast<UserData *>(target));
    }

    static int destroy(lua_State *L)
    {
        auto self = SharedPtrUserData<T, U>::from(lua_touserdata(L, -1));

        assert(!self->isWeak);

        self->target.reset();
        lua_pop(L, 1);
        return 0;
    }
};

}  // namespace chatterino::lua::api

#endif

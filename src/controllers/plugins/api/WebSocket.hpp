// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once
#ifdef CHATTERINO_HAVE_PLUGINS

#    include "common/websockets/WebSocketPool.hpp"

#    include <sol/protected_function.hpp>
#    include <sol/types.hpp>

namespace chatterino {
class Plugin;
}

namespace chatterino::lua::api {

class WebSocket
{
public:
    WebSocket();

    static void createUserType(sol::table &c2, Plugin *plugin);

    void close();

    void sendText(const QByteArray &data);

    void sendBinary(const QByteArray &data);

private:
    sol::main_function onClose;

    sol::main_function onText;

    sol::main_function onBinary;

    sol::main_function onOpen;
    WebSocketHandle handle;

    Plugin *plugin = nullptr;

    friend class WebSocketListenerProxy;
};

}  // namespace chatterino::lua::api

#endif

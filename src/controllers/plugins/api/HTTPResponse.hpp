// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once
#ifdef CHATTERINO_HAVE_PLUGINS
#    include "common/network/NetworkResult.hpp"

#    include <lua.h>
#    include <sol/sol.hpp>

#    include <memory>

namespace chatterino {
class PluginController;
}

namespace chatterino::lua::api {

class HTTPResponse
{
    NetworkResult result_;

public:
    HTTPResponse(NetworkResult res);
    HTTPResponse(HTTPResponse &&other) = default;
    HTTPResponse &operator=(HTTPResponse &&) = default;
    HTTPResponse &operator=(HTTPResponse &) = delete;
    HTTPResponse(const HTTPResponse &other) = delete;
    ~HTTPResponse() = default;

private:
    static void createUserType(sol::table &c2);
    friend class chatterino::PluginController;

public:

    QByteArray data();

    std::optional<int> status();

    QString error();

    QString to_string();
};

}
#endif

#pragma once

#ifdef CHATTERINO_HAVE_PLUGINS

#    include <pajlada/signals/scoped-connection.hpp>
#    include <sol/forward.hpp>

#    include <memory>

namespace chatterino::lua::api {

struct ConnectionHandle {
    std::weak_ptr<pajlada::Signals::ScopedConnection> connection;

    static void createUserType(sol::table &c2);
};

}  // namespace chatterino::lua::api

#endif

#pragma once

#ifdef CHATTERINO_HAVE_PLUGINS

#    include "controllers/plugins/api/ConnectionHandle.hpp"

#    include <pajlada/signals/scoped-connection.hpp>

#    include <vector>

namespace chatterino::lua {

class PluginWeakRef;

class ConnectionManager
{
public:
    ConnectionManager() = default;

    api::ConnectionHandle add(pajlada::Signals::ScopedConnection &&conn);

    template <typename Signal, typename Callback>
    api::ConnectionHandle managedConnect(Signal &signal, Callback cb)
    {
        return this->add(signal.connect(std::forward<Callback>(cb)));
    }

    void clear();

private:
    void removeDisconnected();

    std::vector<std::shared_ptr<pajlada::Signals::ScopedConnection>>
        connections;
};

}  // namespace chatterino::lua

#endif

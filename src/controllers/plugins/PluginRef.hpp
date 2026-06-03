#pragma once

#ifdef CHATTERINO_HAVE_PLUGINS

#    include <memory>

namespace chatterino {
class Plugin;
}

namespace chatterino::lua {

class PluginWeakRef;

class PluginRef
{
public:
    PluginRef() = default;
    PluginRef(PluginRef &&) = default;
    PluginRef &operator=(const PluginRef &) = default;
    PluginRef &operator=(PluginRef &&) = default;
    PluginRef(const PluginRef &) = default;
    ~PluginRef() = default;

    PluginWeakRef weak() const noexcept;
    Plugin *plugin() const noexcept;

    operator bool() const noexcept
    {
        return this->shared != nullptr;
    }

private:
    PluginRef(Plugin *plugin);
    PluginRef(std::shared_ptr<Plugin> plugin);

    void destroy();

    std::shared_ptr<Plugin> shared;

    friend Plugin;
    friend PluginWeakRef;
};

class PluginWeakRef
{
public:
    PluginWeakRef() = default;
    PluginWeakRef(const PluginWeakRef &) = default;
    PluginWeakRef(PluginWeakRef &&) = default;
    PluginWeakRef &operator=(const PluginWeakRef &) = default;
    PluginWeakRef &operator=(PluginWeakRef &&) = default;
    ~PluginWeakRef() = default;

    PluginRef strong() const noexcept;

    bool isAlive() const noexcept;

private:
    PluginWeakRef(std::weak_ptr<Plugin> weak);

    std::weak_ptr<Plugin> weak;

    friend PluginRef;
};

}  // namespace chatterino::lua

#endif

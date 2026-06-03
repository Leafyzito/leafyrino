// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once
#ifdef CHATTERINO_HAVE_PLUGINS
#    include "common/Channel.hpp"
#    include "providers/twitch/TwitchChannel.hpp"

#    include <sol/forward.hpp>

namespace chatterino::lua {
class ThisPluginState;
}

namespace chatterino::lua::api {

struct ConnectionHandle;

struct ChannelRef {
public:
    ChannelRef(const std::shared_ptr<Channel> &chan);

    bool is_valid();

    QString get_name();

    Channel::Type get_type();

    QString get_display_name();

    void send_message(QString text, sol::variadic_args va);

    void add_system_message(QString text);

    void add_message(std::shared_ptr<Message> &message, sol::variadic_args va);

    std::vector<MessagePtrMut> message_snapshot(size_t n_items);

    MessagePtrMut last_message();

    void replace_message(const MessagePtrMut &message,
                         const MessagePtrMut &replacement);

    void replace_message_hint(const MessagePtrMut &message,
                              const MessagePtrMut &replacement, size_t hint);

    void replace_message_at(size_t index, const MessagePtrMut &replacement);

    void clear_messages();

    MessagePtrMut find_message_by_id(const QString &id);

    bool has_messages();

    size_t count_messages();

    bool is_twitch_channel();

    sol::table get_room_modes(sol::this_state state);

    sol::table get_stream_status(sol::this_state state);

    QString get_twitch_id();

    bool is_broadcaster();

    bool is_mod();

    bool is_vip();

    QString to_string();

    bool operator==(const ChannelRef &other) const noexcept;

    ConnectionHandle on_display_name_changed(ThisPluginState state,
                                             sol::main_protected_function pfn);

    static std::optional<ChannelRef> get_by_name(const QString &name);

    static std::optional<ChannelRef> get_by_twitch_id(const QString &id);

    static void createUserType(sol::table &c2);

private:
    std::weak_ptr<Channel> weak;

    std::shared_ptr<Channel> strong();

    std::shared_ptr<TwitchChannel> twitch();
};

sol::table toTable(lua_State *L, const TwitchChannel::RoomModes &modes);
sol::table toTable(lua_State *L, const TwitchChannel::StreamStatus &status);

}
#endif

// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once
#ifdef CHATTERINO_HAVE_PLUGINS
#    include "messages/Link.hpp"
#    include "messages/Message.hpp"

#    include <sol/forward.hpp>

#    include <cstdint>

namespace chatterino::lua::api::message {

enum class ExposedLinkType : std::uint8_t {
    Url = Link::Type::Url,
    UserInfo = Link::Type::UserInfo,
    UserAction = Link::Type::UserAction,
    JumpToChannel = Link::Type::JumpToChannel,
    CopyToClipboard = Link::Type::CopyToClipboard,
    JumpToMessage = Link::Type::JumpToMessage,
    InsertText = Link::Type::InsertText,
};

void createUserType(sol::table &c2);

}  // namespace chatterino::lua::api::message

#endif

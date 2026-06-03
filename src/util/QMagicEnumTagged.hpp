// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/QMagicEnum.hpp"

#include <optional>

namespace chatterino::qmagicenum {

using customize_t = std::optional<std::string_view>;

namespace detail {

namespace tag {

struct DisplayName {
};

}  // namespace tag

template <typename E, typename Tag, E V>
constexpr auto buildEnumValueTaggedData() noexcept
{
    [[maybe_unused]] constexpr auto custom = [] {
        static_assert(
            std::is_same_v<Tag, tag::DisplayName>,
            "unhandled tag in QMagicEnumTagged.hpp::enumTaggedDataValue");

        if constexpr (std::is_same_v<Tag, tag::DisplayName>)
        {
            return qmagicenumDisplayName(V);
        }
    }();

    if constexpr (custom.has_value())
    {
        constexpr auto name = *custom;
        static_assert(!name.empty(),
                      "qmagicenum::customize must return a non-empty string.");
        return magic_enum::detail::static_str<name.size()>{name};
    }
    else
    {
        return magic_enum::detail::enum_name_v<E, V>;
    }
}

template <typename E, typename Tag, E V>
inline constexpr auto TAGGED_DATA_STORAGE =
    buildEnumValueTaggedData<E, Tag, V>();

template <typename C, typename E, typename Tag, E V>
consteval auto enumTaggedDataStorage()
{
    constexpr std::string_view utf8 =
        TAGGED_DATA_STORAGE<decltype(V), Tag, V>.str();

    static_assert(isLatin1<utf8.size()>(utf8),
                  "Can't convert non-latin1 UTF8 to UTF16");

    std::array<C, utf8.size() + 1> storage;
    for (std::size_t i = 0; i < utf8.size(); i++)
    {
        storage[i] = static_cast<C>(utf8[i]);
    }
    storage[utf8.size()] = 0;
    return storage;
}

template <typename E, typename Tag, E V>
inline constexpr auto TAGGED_DATA =
    enumTaggedDataStorage<char16_t, E, Tag, V>();

template <typename E, typename Tag, std::size_t... I>
consteval auto taggedDataStorage(std::index_sequence<I...>)
{
    return std::array<QStringView, sizeof...(I)>{{detail::fromArray(
        TAGGED_DATA<E, Tag, magic_enum::enum_values<E>()[I]>)...}};
}

template <typename E, typename Tag>
inline constexpr auto INDEXED_TAGGED_DATA = taggedDataStorage<E, Tag>(
    std::make_index_sequence<magic_enum::enum_count<E>()>{});

template <detail::IsEnum auto V, typename Tag>
[[nodiscard]] consteval QStringView enumTaggedData() noexcept
{
    return QStringView{
        detail::fromArray(detail::TAGGED_DATA<decltype(V), Tag, V>)};
}

template <detail::IsEnum E, typename Tag>
[[nodiscard]] constexpr QStringView enumTaggedData(E value) noexcept
{
    using D = std::decay_t<E>;

    if (const auto i = magic_enum::enum_index<D>(value))
    {
        return detail::INDEXED_TAGGED_DATA<D, Tag>[*i];
    }
    return {};
}

}  // namespace detail

template <detail::IsEnum auto V>
[[nodiscard]] consteval QStringView enumDisplayName() noexcept
{
    if constexpr (requires { qmagicenumDisplayName(V); })
    {
        return detail::enumTaggedData<V, detail::tag::DisplayName>();
    }

    return enumName<V>();
}

template <detail::IsEnum E>
[[nodiscard]] constexpr QStringView enumDisplayName(E value) noexcept
{
    if constexpr (requires { qmagicenumDisplayName(value); })
    {
        using D = std::decay_t<E>;

        return detail::enumTaggedData<D, detail::tag::DisplayName>(value);
    }

    return enumName(value);
}

template <detail::IsEnum auto V>
[[nodiscard]] inline QString enumDisplayNameString() noexcept
{
    return detail::staticString(enumDisplayName<V>());
}

template <detail::IsEnum E>
[[nodiscard]] inline QString enumDisplayNameString(E value) noexcept
{
    using D = std::decay_t<E>;

    return detail::staticString(enumDisplayName<D>(value));
}

}  // namespace chatterino::qmagicenum

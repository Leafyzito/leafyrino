// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <magic_enum/magic_enum.hpp>
#include <QString>
#include <QStringView>

#include <concepts>

namespace chatterino::qmagicenum::detail {

template <typename T, typename U>
concept DecaysTo = std::same_as<std::decay_t<T>, U>;

template <typename T>
concept IsEnum = std::is_enum_v<T>;

template <typename BinaryPredicate>
consteval bool isDefaultPredicate() noexcept
{
    return std::is_same_v<std::decay_t<BinaryPredicate>,
                          std::equal_to<QChar>> ||
           std::is_same_v<std::decay_t<BinaryPredicate>, std::equal_to<>>;
}

template <typename BinaryPredicate>
consteval bool isNothrowInvocable()
{
    return isDefaultPredicate<BinaryPredicate>() ||
           std::is_nothrow_invocable_r_v<bool, BinaryPredicate, QChar, QChar>;
}

template <std::size_t N>
consteval QStringView fromArray(const std::array<char16_t, N> &arr)
{
    return QStringView{arr.data(), static_cast<QStringView::size_type>(N - 1)};
}

template <std::size_t N>
consteval bool isLatin1(std::string_view maybe)
{
    for (std::size_t i = 0; i < N; i++)
    {
        if (maybe[i] < 0x20 || maybe[i] > 0x7e)
        {
            return false;
        }
    }
    return true;
}

template <typename BinaryPredicate>
constexpr bool eq(
    QStringView a, QStringView b,
    [[maybe_unused]] BinaryPredicate &&
        p) noexcept(isNothrowInvocable<BinaryPredicate>())
{

    if (a.size() != b.size())
    {
        return false;
    }

    for (QStringView::size_type i = 0; i < a.size(); i++)
    {
        if (!p(a[i], b[i]))
        {
            return false;
        }
    }

    return true;
}

template <typename C, typename E, E V>
consteval auto enumNameStorage()
{
    constexpr auto utf8 = magic_enum::enum_name<V>();

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

template <typename E, E V>
inline constexpr auto ENUM_NAME_STORAGE = enumNameStorage<char16_t, E, V>();

template <typename E, std::size_t... I>
consteval auto namesStorage(std::index_sequence<I...> )
{
    return std::array<QStringView, sizeof...(I)>{{detail::fromArray(
        ENUM_NAME_STORAGE<E, magic_enum::enum_values<E>()[I]>)...}};
}

template <typename E>
inline constexpr auto NAMES_STORAGE =
    namesStorage<E>(std::make_index_sequence<magic_enum::enum_count<E>()>{});

template <typename Op = std::equal_to<>>
class CaseInsensitive
{
    static constexpr QChar toLower(QChar c) noexcept
    {
        return (c >= u'A' && c <= u'Z')
                   ? QChar(c.unicode() + static_cast<char16_t>(u'a' - u'A'))
                   : c;
    }

public:
    template <DecaysTo<QChar> L, DecaysTo<QChar> R>
    constexpr bool operator()(L lhs, R rhs) const noexcept
    {
        return Op{}(toLower(lhs), toLower(rhs));
    }
};

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
[[nodiscard]] inline QString staticString(QStringView view) noexcept
{

    return QString(QStringPrivate(nullptr, const_cast<char16_t *>(view.utf16()),
                                  view.size()));
}
#else
[[nodiscard]] inline QString staticString(QStringView view)
{
    return view.toString();
}
#endif

}

namespace chatterino::qmagicenum {

template <detail::IsEnum auto V>
[[nodiscard]] consteval QStringView enumName() noexcept
{
    return QStringView{
        detail::fromArray(detail::ENUM_NAME_STORAGE<decltype(V), V>)};
}

template <detail::IsEnum E>
[[nodiscard]] constexpr QStringView enumName(E value) noexcept
{
    using D = std::decay_t<E>;

    if (const auto i = magic_enum::enum_index<D>(value))
    {
        return detail::NAMES_STORAGE<D>[*i];
    }
    return {};
}

template <detail::IsEnum auto V>
[[nodiscard]] inline QString enumNameString() noexcept
{
    return detail::staticString(enumName<V>());
}

template <detail::IsEnum E>
[[nodiscard]] inline QString enumNameString(E value) noexcept
{
    using D = std::decay_t<E>;

    return detail::staticString(enumName<D>(value));
}

template <detail::IsEnum E, typename BinaryPredicate = std::equal_to<>>
[[nodiscard]] constexpr std::optional<std::decay_t<E>>
    enumCast(QStringView name, BinaryPredicate p = {}) noexcept(
        detail::isNothrowInvocable<BinaryPredicate>())
    requires std::is_invocable_r_v<bool, BinaryPredicate, QChar, QChar>
{
    using D = std::decay_t<E>;

    if constexpr (magic_enum::enum_count<D>() == 0)
    {
        static_cast<void>(name);
        return std::nullopt;
    }

    for (std::size_t i = 0; i < magic_enum::enum_count<D>(); i++)
    {
        if (detail::eq(name, detail::NAMES_STORAGE<D>[i], p))
        {
            return magic_enum::enum_value<D>(i);
        }
    }
    return std::nullopt;
}

template <detail::IsEnum E>
[[nodiscard]] inline QString enumFlagsName(E flags, char16_t sep = u'|')
{
    using D = std::decay_t<E>;
    using U = std::underlying_type_t<D>;
    static_assert(magic_enum::detail::subtype_v<E> ==
                      magic_enum::detail::enum_subtype::flags,
                  "enumFlagsName used for non-flags enum");

    QString name;
    auto checkValue = U{0};
    for (std::size_t i = 0; i < magic_enum::enum_count<D>(); ++i)
    {
        const auto v = static_cast<U>(magic_enum::enum_value<D>(i));
        if ((static_cast<U>(flags) & v) != 0)
        {
            const auto n = detail::NAMES_STORAGE<D>[i];
            if (!n.empty())
            {
                checkValue |= v;
                if (!name.isEmpty())
                {
                    name.append(sep);
                }
                name.append(n);
            }
            else
            {
                return {};
            }
        }
    }

    if (checkValue != 0 && checkValue == static_cast<U>(flags))
    {
        return name;
    }
    return {};
}

template <detail::IsEnum E>
[[nodiscard]] constexpr auto enumNames() noexcept
{
    return detail::NAMES_STORAGE<std::decay_t<E>>;
}

inline constexpr auto CASE_INSENSITIVE = detail::CaseInsensitive<>{};

}

// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace chatterino {

template <typename Fn>
class FunctionRef;

template <typename Ret, typename... Params>
class FunctionRef<Ret(Params...)>
{
public:
    FunctionRef() = default;
    FunctionRef(std::nullptr_t)
    {
    }

    template <typename Callable>
    FunctionRef(Callable &&callable)
        requires

        (!std::same_as<std::remove_cvref_t<Callable>, FunctionRef>) &&

            (std::is_void_v<Ret> ||
             std::convertible_to<
                 decltype(std::declval<Callable>()(std::declval<Params>()...)),
                 Ret>)
        : callback(callTrampoline<std::remove_reference_t<Callable>>)

        , callable(reinterpret_cast<uintptr_t>(&callable))
    {
    }

    Ret operator()(Params... params) const
    {
        return this->callback(this->callable, std::forward<Params>(params)...);
    }

    explicit operator bool() const
    {
        return this->callback != nullptr;
    }

    bool operator==(const FunctionRef &other) const
    {
        return this->callback == other.callback &&
               this->callable == other.callable;
    }

    bool operator!=(const FunctionRef &other) const = default;

private:

    using Callback = Ret(uintptr_t, Params...);

    Callback *callback = nullptr;

    uintptr_t callable = 0;

    template <typename Callable>
    static Ret callTrampoline(uintptr_t callable, Params... params)
    {

        return (*reinterpret_cast<Callable *>(callable))(
            std::forward<Params>(params)...);
    }
};

}

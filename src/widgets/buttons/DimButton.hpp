// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/buttons/Button.hpp"

namespace chatterino {

class DimButton : public Button
{
public:
    enum class Dim : std::uint8_t {

        None,

        Some,

        Lots,
    };

    DimButton(BaseWidget *parent = nullptr);

    [[nodiscard]] Dim dim() const noexcept;

    void setDim(Dim value);

    [[nodiscard]] qreal currentContentOpacity() const noexcept;

private:
    Dim dim_ = Dim::Some;
};

}

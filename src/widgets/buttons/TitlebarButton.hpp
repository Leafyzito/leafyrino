// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/buttons/DimButton.hpp"

namespace chatterino {

enum class TitleBarButtonStyle : std::uint8_t {
    None,
    Minimize,
    Maximize,
    Unmaximize,
    Close,
    Settings,
};

class TitleBarButton : public DimButton
{
public:
    TitleBarButton(TitleBarButtonStyle style = {});

    TitleBarButtonStyle getButtonStyle() const;
    void setButtonStyle(TitleBarButtonStyle style);

    void ncEnter();

    void ncLeave();

    void ncMove(QPoint at);

    void ncMousePress(QPoint at);

    void ncMouseRelease(QPoint at);

protected:
    void themeChangedEvent() override;

    void paintContent(QPainter &painter) override;

private:
    TitleBarButtonStyle style_{};
};

}

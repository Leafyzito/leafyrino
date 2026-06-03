// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/buttons/Button.hpp"

#include <QWidget>

#include <optional>

namespace chatterino {

class DrawnButton : public Button
{
    Q_OBJECT

public:
    enum class Symbol : std::uint8_t {

        Plus,

        Kebab,
    };

    struct Options {

        std::optional<int> padding;

        std::optional<int> thickness;

        std::optional<QColor> background;
        std::optional<QColor> backgroundHover;

        std::optional<QColor> foreground;
        std::optional<QColor> foregroundHover;
    };

    DrawnButton(Symbol symbol_, Options options_, BaseWidget *parent);

    void setOptions(Options options_);

protected:
    void themeChangedEvent() override;

    void mouseOverUpdated() override;

    void paintContent(QPainter &painter) override;

private:

    int getPadding() const;

    int getThickness() const;

    QColor getBackground() const;

    QColor getBackgroundHover() const;

    QColor getForeground() const;

    QColor getForegroundHover() const;

    Options options;

    Options symbolOptions;

    const Symbol symbol;
};

}

// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/buttons/Button.hpp"

#include <QColor>
#include <QString>

#include <optional>

class QSvgRenderer;

namespace chatterino {

class SvgButton : public Button
{
    Q_OBJECT

public:

    struct Src {

        QString dark;

        QString light;
    };

    [[nodiscard]] SvgButton(Src source, BaseWidget *parent = nullptr,
                            QSize padding = {6, 3});

    [[nodiscard]] Src source() const;

    void setSource(Src source);

    void setColor(std::optional<QColor> color);

    [[nodiscard]] QSize padding() const;

    void setPadding(QSize padding);

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    void resizeEvent(QResizeEvent *e) override;

    void paintContent(QPainter &painter) override;

private:
    [[nodiscard]] QString currentSvgPath() const;

    void loadSource();

    Src source_;
    QSvgRenderer *svg_;
    QSize padding_;
    std::optional<QColor> color_;
};

}

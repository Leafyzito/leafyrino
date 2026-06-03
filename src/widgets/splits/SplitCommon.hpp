// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QColor>
#include <QString>

#include <algorithm>

namespace chatterino {

/// Pixel width for the leading icon in split header strips (pinned message,
/// prediction, etc.). Shared so titles align when panels stack.
inline int splitHeaderIconColumnWidth(float scale)
{
    const float s = std::max(1.0f, scale);
    return static_cast<int>(std::clamp(18.0f * s, 16.0f, 24.0f));
}

/// Shared QSS for prediction outcome buttons and poll vote rows (accent border, translucent fill).
inline QString splitAccentActionButtonStyleSheet(const QColor &accent,
                                                 const QColor &labelColor,
                                                 bool enabled)
{
    const QString accentCss = accent.name(QColor::HexArgb);
    const QString labelCss = labelColor.name(QColor::HexArgb);
    if (!enabled)
    {
        const QColor dimText =
            QColor::fromRgbF(labelColor.redF() * 0.5, labelColor.greenF() * 0.5,
                             labelColor.blueF() * 0.5, labelColor.alphaF());
        const QColor dimBorder =
            QColor::fromRgbF(accent.redF() * 0.45 + dimText.redF() * 0.25,
                             accent.greenF() * 0.45 + dimText.greenF() * 0.25,
                             accent.blueF() * 0.45 + dimText.blueF() * 0.25,
                             std::max(0.35f, accent.alphaF()));
        return QStringLiteral(
                   "QPushButton { color: %1; text-decoration: none; "
                   "border: 2px solid %2; border-radius: 4px; padding: 4px "
                   "8px; "
                   "background-color: transparent; font-style: italic; }")
            .arg(dimText.name(QColor::HexArgb),
                 dimBorder.name(QColor::HexArgb));
    }
    const int r = accent.red(), g = accent.green(), b = accent.blue();
    const QString bg1 = QColor(r, g, b, 55).name(QColor::HexArgb);
    const QString bg2 = QColor(r, g, b, 90).name(QColor::HexArgb);
    return QStringLiteral(
               "QPushButton { color: %1; text-decoration: none; "
               "border: 2px solid %2; border-radius: 4px; padding: 4px 8px; "
               "background-color: %3; font-weight: 600; }"
               "QPushButton:hover { background-color: %4; }")
        .arg(labelCss, accentCss, bg1, bg2);
}

enum class SplitDirection {
    Left,
    Above,
    Right,
    Below,
};

}

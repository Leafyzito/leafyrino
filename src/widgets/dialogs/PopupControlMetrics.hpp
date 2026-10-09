#pragma once

#include <QFontMetrics>

#include <algorithm>

namespace chatterino {

inline int popupControlMinimumHeight(float contentScale)
{
    return std::max(14, int(20 * contentScale));
}

inline int popupControlHeight(const QFont &font, float contentScale)
{
    const auto contentHeight = std::max(
        popupControlMinimumHeight(contentScale),
        QFontMetrics(font).height() + std::max(1, int(2 * contentScale)));
    return contentHeight + 2;
}

}

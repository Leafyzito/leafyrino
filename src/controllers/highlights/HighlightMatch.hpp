#pragma once

#include <QColor>
#include <QString>

#include <cstdint>

namespace chatterino {

enum class HighlightMatchSource : std::uint8_t {
    AutoMod,
};

enum class HighlightMatchStyle : std::uint8_t {
    None,
    Outline,
    Fill,
    OutlineAndFill,
    Underline,
};

/// A range of Message::messageText that should be marked when painted.
///
/// Only AutoMod produces these right now. `start` and `length` are UTF-16
/// offsets into the message text.
struct HighlightMatch {
    qsizetype start{};
    qsizetype length{};
    QColor color;
    /// Shown as a tooltip when hovering the marked text
    QString ruleName;
    QString pattern;
    HighlightMatchSource source{HighlightMatchSource::AutoMod};
    HighlightMatchStyle style{HighlightMatchStyle::Fill};

    bool operator==(const HighlightMatch &other) const = default;
};

QColor defaultAutoModMatchColor();

/// Reads the AutoMod flagged text style from the settings, falling back to
/// Fill if the stored value is out of range
HighlightMatchStyle autoModMatchStyle();

/// Reads the AutoMod flagged text color from the settings, falling back to
/// the default color if the stored value is invalid
QColor autoModMatchColor();

}  // namespace chatterino

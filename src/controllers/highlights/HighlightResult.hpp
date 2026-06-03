// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QColor>
#include <QUrl>

#include <memory>
#include <optional>
#include <ostream>

namespace chatterino {

struct HighlightResult {
    HighlightResult(bool _alert, bool _playSound,
                    std::optional<QUrl> _customSoundUrl,
                    std::shared_ptr<QColor> _color, bool _showInMentions);

    static HighlightResult emptyResult();

    bool alert{false};

    bool playSound{false};

    std::optional<QUrl> customSoundUrl{};

    std::shared_ptr<QColor> color{};

    bool showInMentions{false};

    bool operator==(const HighlightResult &other) const;
    bool operator!=(const HighlightResult &other) const;

    [[nodiscard]] bool empty() const;

    [[nodiscard]] bool full() const;

    friend std::ostream &operator<<(std::ostream &os,
                                    const HighlightResult &result);
};

}  // namespace chatterino

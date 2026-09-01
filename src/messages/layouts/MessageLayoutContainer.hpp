// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Common.hpp"
#include "common/FlagsEnum.hpp"
#include "messages/MessageFlag.hpp"

#include <QPoint>
#include <QRect>

#include <memory>
#include <optional>
#include <vector>

#if __has_include(<gtest/gtest_prod.h>)
#    include <gtest/gtest_prod.h>
#endif

class QPainter;

namespace chatterino {

enum class TextDirection : uint8_t {
    Neutral,
    RTL,
    LTR,
};

class MessageLayoutElement;
struct Selection;
struct MessagePaintContext;

struct MessageLayoutContainer {
    MessageLayoutContainer() = default;

    void beginLayout(qreal width, float scale, float imageScale,
                     float emoteScale, float badgeScale, bool centerBadges,
                     MessageFlags flags);

    void endLayout();

    void addElement(MessageLayoutElement *element);

    void addElementNoLineBreak(MessageLayoutElement *element);

    void ensureSingleSpaceBeforeNextElement();

    void breakLine();

    void paintElements(QPainter &painter, const MessagePaintContext &ctx) const;

    bool paintAnimatedElements(QPainter &painter, qreal yOffset,
                               bool isCollapsed = false) const;

    void paintSelection(QPainter &painter, size_t messageIndex,
                        const Selection &selection, qreal yOffset) const;

    void addSelectionText(QString &str, uint32_t from, uint32_t to,
                          CopyMode copymode) const;

    MessageLayoutElement *getElementAt(QPointF point) const;

    size_t getSelectionIndex(QPointF point) const;

    size_t getFirstMessageCharacterIndex() const;

    std::pair<int, int> getWordBounds(
        const MessageLayoutElement *hoveredElement) const;

    size_t getLastCharacterIndex() const;

    qreal getWidth() const;

    /// Width of the laid-out content (ignoring the layout canvas width).
    qreal getLayoutContentWidth() const;

    qreal getHeight() const;

    int getFirstLineHeight() const;

    float getScale() const;

    float getImageScale() const;

    float getEmoteScale() const;

    float getBadgeScale() const;

    bool isCollapsed() const;

    size_t getLineCount() const;

    bool atStartOfLine() const;

    bool fitsInLine(qreal width) const;

    qreal remainingWidth() const;

    int nextWordId();

private:
    struct Line {
        size_t startIndex{};

        size_t endIndex{};

        size_t startCharIndex{};

        size_t endCharIndex{};

        QRectF rect;
    };

    void addElement(MessageLayoutElement *element, bool forceAdd,
                    qsizetype prevIndex);

    void reorderRTL(size_t firstTextIndex);

    void paintSelectionRect(QPainter &painter, const Line &line, qreal left,
                            qreal right, qreal yOffset,
                            const QColor &color) const;

    std::optional<size_t> paintSelectionStart(QPainter &painter,
                                              size_t messageIndex,
                                              const Selection &selection,
                                              qreal yOffset) const;

    void paintSelectionEnd(QPainter &painter, size_t lineIndex,
                           const Selection &selection, qreal yOffset) const;

    bool canAddElements() const;

    bool canCollapse() const;

    [[nodiscard]] bool isRTL() const noexcept;

    [[nodiscard]] bool isLTR() const noexcept;

    [[nodiscard]] bool isNeutral() const noexcept;

    float scale_ = 1.F;

    float imageScale_ = 1.F;

    float emoteScale_ = 1.F;

    float badgeScale_ = 1.F;

    bool centerBadges_ = false;
    qreal width_ = 0;
    float descent_ = 0.F;
    MessageFlags flags_{};

    size_t line_{};
    qreal height_ = 0;
    qreal currentX_ = 0;
    qreal currentY_ = 0;

    size_t charIndex_ = 0;
    size_t lineStart_ = 0;
    qreal lineHeight_ = 0;
    qreal spaceWidth_ = 4;
    qreal textLineHeight_ = 0;
    qreal dotdotdotWidth_ = 0;
    int currentWordId_ = 0;
    bool canAddMessages_ = true;
    bool isCollapsed_ = false;

    bool lineContainsRTL_ = false;

    bool anyReorderingDone_ = false;

    TextDirection textDirection_ = TextDirection::Neutral;

    std::vector<std::unique_ptr<MessageLayoutElement>> elements_;

    std::vector<Line> lines_;

#ifdef FRIEND_TEST
    FRIEND_TEST(MessageLayoutContainerTest, RtlReordering);
    FRIEND_TEST(MessageLayoutContainer, ExhaustiveFlags);
#endif
};

}  // namespace chatterino

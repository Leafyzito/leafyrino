// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Common.hpp"
#include "common/FlagsEnum.hpp"
#include "messages/layouts/MessageLayoutContainer.hpp"

#include <QPixmap>

#include <cinttypes>
#include <memory>

namespace chatterino {

struct Message;
using MessagePtr = std::shared_ptr<const Message>;

struct Selection;
struct MessageLayoutContainer;
class MessageLayoutElement;
struct MessagePaintContext;
struct MessageLayoutContext;

enum class MessageElementFlag : int64_t;
using MessageElementFlags = FlagsEnum<MessageElementFlag>;

enum class MessageLayoutFlag : uint8_t {
    RequiresBufferUpdate = 1 << 1,
    RequiresLayout = 1 << 2,
    AlternateBackground = 1 << 3,
    Collapsed = 1 << 4,
    Expanded = 1 << 5,
    IgnoreHighlights = 1 << 6,
};
using MessageLayoutFlags = FlagsEnum<MessageLayoutFlag>;

struct MessagePaintResult {
    bool hasAnimatedElements = false;
};

class MessageLayout
{
public:
    MessageLayout(MessagePtr message_);
    ~MessageLayout();

    MessageLayout(const MessageLayout &) = delete;
    MessageLayout &operator=(const MessageLayout &) = delete;

    MessageLayout(MessageLayout &&) = delete;
    MessageLayout &operator=(MessageLayout &&) = delete;

    const Message *getMessage();
    const MessagePtr &getMessagePtr() const;

    int getHeight() const;
    int getFirstLineHeight() const;
    int getWidth() const;
    int getLayoutContentWidth() const;
    size_t getLineCount() const;

    MessageLayoutFlags flags;

    bool layout(const MessageLayoutContext &ctx, bool shouldInvalidateBuffer);

    MessagePaintResult paint(const MessagePaintContext &ctx);
    void invalidateBuffer();
    void deleteBuffer();
    void deleteCache();

    const MessageLayoutElement *getElementAt(QPointF point) const;

    std::pair<int, int> getWordBounds(
        const MessageLayoutElement *hoveredElement, QPointF relativePos) const;

    size_t getLastCharacterIndex() const;

    size_t getFirstMessageCharacterIndex() const;

    size_t getSelectionIndex(QPointF position) const;
    void addSelectionText(QString &str, uint32_t from = 0,
                          uint32_t to = UINT32_MAX,
                          CopyMode copymode = CopyMode::Everything);

    bool isDisabled() const;

private:
    void actuallyLayout(const MessageLayoutContext &ctx);
    void updateBuffer(QPixmap *buffer, const MessagePaintContext &ctx);

    QPixmap *ensureBuffer(QPainter &painter, qreal width, bool clear);

    const MessagePtr message_;
    MessageLayoutContainer container_;
    std::unique_ptr<QPixmap> buffer_;
    bool bufferValid_ = false;

    qreal height_ = 0;
    int currentLayoutWidth_ = -1;
    int layoutState_ = -1;
    float scale_ = -1;
    float imageScale_ = -1.F;
    float emoteScale_ = -1.F;
    float badgeScale_ = -1.F;
    bool centerBadges_ = false;
    MessageElementFlags currentWordFlags_;

#ifdef FOURTF

    unsigned int layoutCount_ = 0;
    unsigned int bufferUpdatedCount_ = 0;
#endif
};

using MessageLayoutPtr = std::shared_ptr<MessageLayout>;

}  // namespace chatterino

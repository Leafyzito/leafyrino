// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/layouts/MessageLayout.hpp"

#include "Application.hpp"
#include "controllers/highlights/types/AutomodCaughtHighlight.hpp"
#include "messages/layouts/MessageLayoutContainer.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"
#include "messages/Selection.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/WindowManager.hpp"
#include "util/DebugCount.hpp"

#include <QApplication>
#include <QDebug>
#include <QPainter>
#include <QtGlobal>
#include <QThread>

#include <cassert>
#include <optional>

namespace chatterino {

namespace {

constexpr int SCROLLBAR_PADDING = 20;

std::optional<QColor> clientDetectionHighlightColor(
    Message::ClientDetectionStatus status,
    const MessagePreferences &preferences)
{
    if (!preferences.enableClientDetectionHighlight)
    {
        return std::nullopt;
    }

    switch (status)
    {
        case Message::ClientDetectionStatus::Web:
            return preferences.clientDetectionWebColor;
        case Message::ClientDetectionStatus::Android:
            return preferences.clientDetectionAndroidColor;
        case Message::ClientDetectionStatus::IOS:
            return preferences.clientDetectionIosColor;
        case Message::ClientDetectionStatus::MobileNew:
            return preferences.clientDetectionMobileNewColor;
        case Message::ClientDetectionStatus::Unknown:
        case Message::ClientDetectionStatus::Abnormal:
            return std::nullopt;
    }

    return std::nullopt;
}

QColor blendColors(const QColor &base, const QColor &apply)
{
    const qreal &alpha = apply.alphaF();
    QColor result;
    result.setRgbF(base.redF() * (1 - alpha) + apply.redF() * alpha,
                   base.greenF() * (1 - alpha) + apply.greenF() * alpha,
                   base.blueF() * (1 - alpha) + apply.blueF() * alpha);
    return result;
}

QColor platformTint(MessagePlatform platform)
{
    constexpr int a = 22;
    switch (platform)
    {
        case MessagePlatform::AnyOrTwitch:
            return {145, 70, 255, a};
        case MessagePlatform::YouTube:
            return {255, 0, 0, a};
        case MessagePlatform::Kick:
            return {83, 252, 24, a};
    }
    return Qt::transparent;
}
}  // namespace

MessageLayout::MessageLayout(MessagePtr message)
    : message_(std::move(message))
{
    DebugCount::increase(DebugObject::MessageLayout);
}

MessageLayout::~MessageLayout()
{
    this->deleteBuffer();
    DebugCount::decrease(DebugObject::MessageLayout);
}

const Message *MessageLayout::getMessage()
{
    return this->message_.get();
}

const MessagePtr &MessageLayout::getMessagePtr() const
{
    return this->message_;
}

int MessageLayout::getHeight() const
{
    return static_cast<int>(this->height_);
}

int MessageLayout::getFirstLineHeight() const
{
    return this->firstLineHeight_;
}

int MessageLayout::getWidth() const
{
    return this->width_;
}

int MessageLayout::getLayoutContentWidth() const
{
    return this->layoutContentWidth_;
}

size_t MessageLayout::getLineCount() const
{
    return this->lineCount_;
}

bool MessageLayout::layout(const MessageLayoutContext &ctx,
                           bool shouldInvalidateBuffer)
{
    return this->layoutImpl(ctx, shouldInvalidateBuffer, true);
}

bool MessageLayout::layoutForMeasurement(const MessageLayoutContext &ctx)
{
    return this->layoutImpl(ctx, false, false);
}

bool MessageLayout::layoutImpl(const MessageLayoutContext &ctx,
                               bool shouldInvalidateBuffer,
                               bool retainContainer)
{
    const bool hadContainer = this->container_ != nullptr;
    // the elements were freed by deleteCache(), they have to be recreated
    bool layoutRequired = retainContainer && !hadContainer;

    bool widthChanged = ctx.width != this->currentLayoutWidth_;
    layoutRequired |= widthChanged;
    this->currentLayoutWidth_ = ctx.width;

    const auto layoutGeneration = getApp()->getWindows()->getGeneration();
    if (this->layoutState_ != layoutGeneration)
    {
        layoutRequired = true;
        this->flags.set(MessageLayoutFlag::RequiresBufferUpdate);
        this->layoutState_ = layoutGeneration;
    }

    layoutRequired |= this->currentWordFlags_ != ctx.flags;
    this->currentWordFlags_ = ctx.flags;

    if (this->flags.has(MessageLayoutFlag::AutoModReviewSelected) !=
        ctx.autoModReviewExpanded)
    {
        this->flags.set(MessageLayoutFlag::AutoModReviewSelected,
                        ctx.autoModReviewExpanded);
        this->flags.set(MessageLayoutFlag::RequiresBufferUpdate);
        layoutRequired = true;
    }
    if (this->flags.has(MessageLayoutFlag::AutoModReviewChannel) !=
        ctx.autoModReviewChannel)
    {
        this->flags.set(MessageLayoutFlag::AutoModReviewChannel,
                        ctx.autoModReviewChannel);
        this->flags.set(MessageLayoutFlag::RequiresBufferUpdate);
        layoutRequired = true;
    }

    layoutRequired |= this->flags.has(MessageLayoutFlag::RequiresLayout);
    this->flags.unset(MessageLayoutFlag::RequiresLayout);

    bool scaleChanged = this->scale_ != ctx.scale ||
                        this->imageScale_ != ctx.imageScale ||
                        this->emoteScale_ != ctx.emoteScale ||
                        this->badgeScale_ != ctx.badgeScale ||
                        this->centerBadges_ != ctx.centerBadges;
    layoutRequired |= scaleChanged;
    this->scale_ = ctx.scale;
    this->imageScale_ = ctx.imageScale;
    this->emoteScale_ = ctx.emoteScale;
    this->badgeScale_ = ctx.badgeScale;
    this->centerBadges_ = ctx.centerBadges;

    if (!layoutRequired)
    {
        if (shouldInvalidateBuffer)
        {
            this->invalidateBuffer();
            return true;
        }
        return false;
    }

    qreal oldHeight = this->height_;
    this->actuallyLayout(ctx);
    if (widthChanged || this->height_ != oldHeight)
    {
        this->deleteBuffer();
    }
    this->invalidateBuffer();

    if (!retainContainer && !hadContainer)
    {
        this->container_.reset();
    }

    return true;
}

void MessageLayout::actuallyLayout(const MessageLayoutContext &ctx)
{
#ifdef FOURTF
    this->layoutCount_++;
#endif

    if (!this->container_)
    {
        this->container_ = std::make_unique<MessageLayoutContainer>();
    }
    auto &container = *this->container_;

    auto messageFlags = this->message_->flags;

    if (this->flags.has(MessageLayoutFlag::Expanded) ||
        (ctx.flags.has(MessageElementFlag::ModeratorTools) &&
         !this->message_->flags.has(MessageFlag::Disabled)))
    {
        messageFlags.unset(MessageFlag::Collapsed);
    }

    bool hideModerated = getSettings()->hideModerated;
    bool hideModerationActions = getSettings()->hideModerationActions;
    bool hideBlockedTermAutomodMessages =
        getSettings()->showBlockedTermAutomodMessages.getEnum() ==
        ShowModerationState::Never;
    bool hideSimilar = getSettings()->hideSimilar;
    bool hideReplies = !ctx.flags.has(MessageElementFlag::RepliedMessage);
    const bool hideGigantifyReward =
        this->message_->usesTwitchGigantifyPresentation();

    container.beginLayout(ctx.width, this->scale_, this->imageScale_,
                          this->emoteScale_, this->badgeScale_,
                          this->centerBadges_, messageFlags);

    for (const auto &element : this->message_->elements)
    {
        if (!ctx.autoModReviewExpanded &&
            element->getFlags().has(MessageElementFlag::AutoModReviewExpanded))
        {
            continue;
        }

        if (hideModerated && this->message_->flags.has(MessageFlag::Disabled))
        {
            continue;
        }

        if (hideBlockedTermAutomodMessages &&
            this->message_->flags.has(MessageFlag::AutoModBlockedTerm))
        {
            continue;
        }

        if (this->message_->flags.has(MessageFlag::RestrictedMessage))
        {
            if (getApp()->getStreamerMode()->shouldHideRestrictedUsers())
            {
                continue;
            }
        }

        if (this->message_->flags.has(MessageFlag::ModerationAction))
        {
            if (hideModerationActions ||
                getApp()->getStreamerMode()->shouldHideModActions())
            {
                continue;
            }
        }

        if (hideSimilar && this->message_->flags.has(MessageFlag::Similar))
        {
            continue;
        }

        if (hideReplies &&
            element->getFlags().has(MessageElementFlag::RepliedMessage))
        {
            continue;
        }

        if (hideGigantifyReward &&
            element->getFlags().has(MessageElementFlag::ChannelPointReward))
        {
            continue;
        }

        element->addToContainer(container, ctx);
    }

    if (this->height_ != container.getHeight())
    {
        this->deleteBuffer();
    }

    container.endLayout();
    this->height_ = container.getHeight();
    this->firstLineHeight_ = container.getFirstLineHeight();
    this->width_ = static_cast<int>(container.getWidth());
    this->layoutContentWidth_ =
        static_cast<int>(std::ceil(container.getLayoutContentWidth()));
    this->lineCount_ = container.getLineCount();
    this->firstMessageCharacterIndex_ =
        container.getFirstMessageCharacterIndex();
    this->lastCharacterIndex_ = container.getLastCharacterIndex();

    this->flags.unset(MessageLayoutFlag::Collapsed);
    if (container.isCollapsed())
    {
        this->flags.set(MessageLayoutFlag::Collapsed);
    }
}

MessagePaintResult MessageLayout::paint(const MessagePaintContext &ctx)
{
    MessagePaintResult result;
    if (!this->container_)
    {
        return result;
    }
    auto &container = *this->container_;

    QPixmap *pixmap = this->ensureBuffer(ctx.painter, ctx.canvasWidth,
                                         ctx.messageColors.hasTransparency);

    if (!this->bufferValid_)
    {
        if (ctx.messageColors.hasTransparency)
        {
            pixmap->fill(Qt::transparent);
        }
        this->updateBuffer(pixmap, ctx);
    }

    ctx.painter.drawPixmap(QPoint{0, ctx.y}, *pixmap);

    result.animatedRegion =
        container.paintAnimatedElements(ctx.painter, ctx.y, ctx.isCollapsed);

    if (this->message_->flags.has(MessageFlag::Disabled))
    {
        ctx.painter.fillRect(
            QRect{
                0,
                ctx.y,
                pixmap->width(),
                pixmap->height(),
            },
            ctx.messageColors.disabled);
    }

    if (this->message_->flags.has(MessageFlag::RecentMessage) &&
        ctx.preferences.fadeMessageHistory)
    {
        ctx.painter.fillRect(
            QRect{
                0,
                ctx.y,
                pixmap->width(),
                pixmap->height(),
            },
            ctx.messageColors.disabled);
    }

    // draw selection
    if (!ctx.selection.isEmpty())
    {
        container.paintSelection(ctx.painter, ctx.messageIndex, ctx.selection,
                                 ctx.y);
    }

    if (ctx.preferences.separateMessages)
    {
        ctx.painter.fillRect(
            QRectF{
                0.0,
                static_cast<qreal>(ctx.y),
                static_cast<qreal>(ctx.canvasWidth),
                1.0,
            },
            ctx.messageColors.messageSeperator);
    }

    if (ctx.isLastReadMessage)
    {
        QColor color;
        if (ctx.preferences.lastMessageColor.isValid())
        {
            color = ctx.preferences.lastMessageColor;
        }
        else
        {
            color = ctx.isWindowFocused
                        ? ctx.messageColors.focusedLastMessageLine
                        : ctx.messageColors.unfocusedLastMessageLine;
        }

        QBrush brush(color, ctx.preferences.lastMessagePattern);

        ctx.painter.fillRect(
            QRectF{
                0,
                ctx.y + this->height_ - 1,
                static_cast<qreal>(pixmap->width()),
                1,
            },
            brush);
    }

    this->bufferValid_ = true;

    return result;
}

QPixmap *MessageLayout::ensureBuffer(QPainter &painter, qreal width, bool clear)
{
    if (this->buffer_ != nullptr)
    {
        return this->buffer_.get();
    }

    this->buffer_ = std::make_unique<QPixmap>(
        static_cast<int>(width * painter.device()->devicePixelRatioF()),
        static_cast<int>(this->height_ *
                         painter.device()->devicePixelRatioF()));
    this->buffer_->setDevicePixelRatio(painter.device()->devicePixelRatioF());

    if (clear)
    {
        this->buffer_->fill(Qt::transparent);
    }

    this->bufferValid_ = false;
    DebugCount::increase(DebugObject::MessageDrawingBuffer);
    return this->buffer_.get();
}

void MessageLayout::updateBuffer(QPixmap *buffer,
                                 const MessagePaintContext &ctx)
{
    if (buffer->isNull() || !this->container_)
    {
        return;
    }

    QPainter painter(buffer);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    QColor backgroundColor = [&] {
        if (ctx.preferences.alternateMessages &&
            this->flags.has(MessageLayoutFlag::AlternateBackground))
        {
            return ctx.messageColors.alternateBg;
        }

        return ctx.messageColors.regularBg;
    }();

    const bool isAutoModReview = this->message_->autoModReview != nullptr;
    const bool isSelectedAutoModReview =
        isAutoModReview &&
        this->flags.has(MessageLayoutFlag::AutoModReviewSelected);

    if (this->message_->flags.has(MessageFlag::Highlighted) &&
        !this->flags.has(MessageLayoutFlag::IgnoreHighlights))
    {
        // NOTE: As we move more things into Highlighted, and less things to use custom flags for their color,
        // they will start respecting the IgnoreHighlights flag. Keep this in mind.
        // assert(this->message_->highlightColor);
        if (this->message_->highlightColor &&
            this->message_->highlightColor->isValid())
        {
            auto highlight = *this->message_->highlightColor;
            // every message in the /automod queue is a review, keep the tint
            // light there so the selection stands out
            if (isAutoModReview &&
                this->flags.has(MessageLayoutFlag::AutoModReviewChannel))
            {
                highlight.setAlpha(std::min(highlight.alpha(), 40));
            }
            backgroundColor = blendColors(backgroundColor, highlight);
        }
    }
    else if (this->message_->flags.has(MessageFlag::ChatWarning))
    {
        backgroundColor = blendColors(backgroundColor, QColor(64, 64, 64, 127));
    }
    else if (this->message_->flags.has(MessageFlag::Debug))
    {
        backgroundColor = QColor("#4A273D");
    }
    else if (const auto clientColor = clientDetectionHighlightColor(
                 this->message_->clientDetection, ctx.preferences))
    {
        backgroundColor = blendColors(backgroundColor, *clientColor);
    }

    if (ctx.tintByPlatform)
    {
        backgroundColor = blendColors(backgroundColor,
                                      platformTint(this->message_->platform));
    }

    std::optional<QPixmap> clientDetectionIcon = {};

    if (getSettings()->clientDetectionIcon)
    {
        auto resources = getResources();

        switch (this->message_->clientDetection)
        {
            using enum Message::ClientDetectionStatus;
            case Webchat: {
                clientDetectionIcon = resources.chat.twitch;
                break;
            }
            case Android: {
                clientDetectionIcon = resources.chat.android;
                break;
            }
            case IOS: {
                clientDetectionIcon = resources.chat.ios;
                break;
            }
            case MobileNew: {
                clientDetectionIcon = resources.chat.mobile;
                break;
            }

            case Unknown:
            case Abnormal:
                break;
        }
    }

    if (isSelectedAutoModReview)
    {
        auto selection = ctx.messageColors.selection;
        selection.setAlpha(26);
        backgroundColor = blendColors(backgroundColor, selection);
    }

    painter.fillRect(buffer->rect(), backgroundColor);

    // accent bar so held messages are easy to spot in chat
    if (isAutoModReview)
    {
        QColor accent;
        if (isSelectedAutoModReview)
        {
            accent = ctx.messageColors.linkText;
        }
        else if (this->message_->highlightColor &&
                 this->message_->highlightColor->isValid())
        {
            accent = *this->message_->highlightColor;
        }
        else
        {
            accent = autoModMatchColor();
        }
        accent.setAlpha(isSelectedAutoModReview ? 205 : 160);
        painter.fillRect(
            QRectF(0, 0,
                   std::max(1.0, (isSelectedAutoModReview ? 2.0 : 1.5) *
                                     static_cast<qreal>(this->scale_)),
                   buffer->height() / buffer->devicePixelRatioF()),
            accent);
    }

    if (getSettings()->clientDetectionIcon && clientDetectionIcon.has_value())
    {
        float size = 16 * this->scale_;
        float right =
            float(buffer->rect().right()) - (SCROLLBAR_PADDING * this->scale_);
        int left = int(right - size);
        int top = int((float(buffer->height()) - size * this->scale_) / 2);

        painter.drawPixmap(QRect(left, top, int(size), int(size)),
                           clientDetectionIcon.value());
    }

    this->container_->paintElements(painter, ctx);

#ifdef FOURTF

    painter.setPen(QColor(255, 0, 0));
    painter.drawRect(buffer->rect().x(), buffer->rect().y(),
                     buffer->rect().width() - 1, buffer->rect().height() - 1);

    QTextOption option;
    option.setAlignment(Qt::AlignRight | Qt::AlignTop);

    painter.drawText(QRectF(1, 1, this->width_ - 3, 1000),
                     QString::number(this->layoutCount_) + ", " +
                         QString::number(++this->bufferUpdatedCount_),
                     option);
#endif
}

void MessageLayout::invalidateBuffer()
{
    this->bufferValid_ = false;
}

void MessageLayout::deleteBuffer()
{
    if (this->buffer_ != nullptr)
    {
        DebugCount::decrease(DebugObject::MessageDrawingBuffer);

        this->buffer_ = nullptr;
    }
}

void MessageLayout::deleteCache()
{
    this->deleteBuffer();
    this->container_.reset();
}

bool MessageLayout::hasCache() const
{
    return this->container_ != nullptr;
}

const MessageLayoutElement *MessageLayout::getElementAt(QPointF point) const
{
    if (!this->container_)
    {
        return nullptr;
    }
    return this->container_->getElementAt(point);
}

std::pair<int, int> MessageLayout::getWordBounds(
    const MessageLayoutElement *hoveredElement, QPointF relativePos) const
{
    // hoveredElement comes from getElementAt(), so the elements are still there
    assert(this->container_);
    if (hoveredElement->getWordId() != -1 && this->container_)
    {
        return this->container_->getWordBounds(hoveredElement);
    }

    const auto wordStart = this->getSelectionIndex(relativePos) -
                           hoveredElement->getMouseOverIndex(relativePos);
    const auto selectionLength = hoveredElement->getSelectionIndexCount();
    const auto length = hoveredElement->hasTrailingSpace() ? selectionLength - 1
                                                           : selectionLength;

    return {wordStart, wordStart + length};
}

size_t MessageLayout::getLastCharacterIndex() const
{
    return this->lastCharacterIndex_;
}

size_t MessageLayout::getFirstMessageCharacterIndex() const
{
    return this->firstMessageCharacterIndex_;
}

size_t MessageLayout::getSelectionIndex(QPointF position) const
{
    if (!this->container_)
    {
        return 0;
    }
    return this->container_->getSelectionIndex(position);
}

void MessageLayout::addSelectionText(QString &str, uint32_t from, uint32_t to,
                                     CopyMode copymode)
{
    if (!this->container_)
    {
        return;
    }
    this->container_->addSelectionText(str, from, to, copymode);
}

}  // namespace chatterino

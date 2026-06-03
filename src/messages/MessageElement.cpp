// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/MessageElement.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/Literals.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "controllers/moderationactions/ModerationAction.hpp"
#include "debug/Benchmark.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/layouts/MessageLayoutContainer.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/Message.hpp"
#include "providers/emoji/Emojis.hpp"
#include "providers/twitch/TwitchEmotes.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "util/DebugCount.hpp"
#include "util/Variant.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QTextLayout>

#include <algorithm>
#include <cmath>
#include <memory>

#ifdef CHATTERINO_WITH_PRIVATE_QT_API
#    include <QtGui/private/qtextengine_p.h>
#endif

namespace chatterino {

using namespace literals;

namespace {

// Computes the bounding box for the given vector of images
QSizeF getBoundingBoxSize(const std::vector<ImagePtr> &images)
{
    qreal width = 0;
    qreal height = 0;
    for (const auto &img : images)
    {
        QSizeF s = img->size();
        width = std::max(width, s.width());
        height = std::max(height, s.height());
    }

    return {width, height};
}

QSizeF textElementSize(const QString &text, const QFontMetricsF &metrics,
                       const QFont *layoutFont)
{
    QSizeF size{
        metrics.horizontalAdvance(text),
        metrics.height(),
    };

    if (layoutFont == nullptr || text.isEmpty())
    {
        return size;
    }

    size.setHeight(std::max(size.height(), metrics.lineSpacing()));

    QTextLayout layout(text, *layoutFont);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid())
    {
        line.setLineWidth(std::max<qreal>(100000.0, size.width() + 100.0));
        size.setWidth(std::max(size.width(), line.naturalTextWidth()));
        size.setHeight(std::max(size.height(), line.height()));
    }
    layout.endLayout();

    const QRectF singleLineBounds = metrics.boundingRect(
        QRectF(0, 0, 100000, 100000),
        Qt::AlignLeft | Qt::AlignTop | Qt::TextSingleLine, text);
    size.setWidth(std::max(size.width(), singleLineBounds.width()));

    return {
        std::ceil(size.width()),
        std::ceil(size.height()),
    };
}

EmotePtr getKickBadge()
{
    static EmotePtr ptr = std::make_shared<const Emote>(Emote{
        .name = {u"Kick"_s},
        .images =
            ImageSet{
                Image::fromUrl({u":/badges/platform-kick-18.webp"_s}, 1.0,
                               {18, 18}),
                Image::fromUrl({u":/badges/platform-kick-36.webp"_s}, .5,
                               {36, 36}),
            },
        .tooltip = Tooltip{},
    });
    return ptr;
}

EmotePtr getTwitchBadge()
{
    static EmotePtr ptr = std::make_shared<const Emote>(Emote{
        .name = {u"Twitch"_s},
        .images =
            ImageSet{
                Image::fromUrl({u":/badges/platform-twitch-18.webp"_s}, 1.0,
                               {18, 18}),
                Image::fromUrl({u":/badges/platform-twitch-36.webp"_s}, .5,
                               {36, 36}),
            },
        .tooltip = Tooltip{},
    });
    return ptr;
}

}  // namespace

MessageElement::MessageElement(MessageElementFlags flags)
    : flags_(flags)
{
    DebugCount::increase(DebugObject::MessageElement);
}

MessageElement::~MessageElement()
{
    DebugCount::decrease(DebugObject::MessageElement);
}

MessageElement *MessageElement::setLink(const Link &link)
{
    this->link_ = link;
    return this;
}

MessageElement *MessageElement::setTooltip(const QString &tooltip)
{
    this->tooltip_ = tooltip;
    return this;
}

MessageElement *MessageElement::setTrailingSpace(bool value)
{
    this->trailingSpace = value;
    return this;
}

const QString &MessageElement::getTooltip() const
{
    return this->tooltip_;
}

Link MessageElement::getLink() const
{
    return this->link_;
}

bool MessageElement::hasTrailingSpace() const
{
    return this->trailingSpace;
}

MessageElementFlags MessageElement::getFlags() const
{
    return this->flags_;
}

void MessageElement::addFlags(MessageElementFlags flags)
{
    this->flags_.set(flags);
}

void MessageElement::cloneFrom(const MessageElement &source)
{
    this->link_ = source.link_;
    this->tooltip_ = source.tooltip_;
    this->flags_ = source.flags_;
    this->trailingSpace = source.trailingSpace;
}

QJsonObject MessageElement::toJson() const
{
    return {
        {"trailingSpace"_L1, this->trailingSpace},
        {
            "link"_L1,
            {{
                {"type"_L1, qmagicenum::enumNameString(this->link_.type)},
                {"value"_L1, this->link_.value},
            }},
        },
        {"tooltip"_L1, this->tooltip_},
        {"flags"_L1, qmagicenum::enumFlagsName(this->flags_.value())},
    };
}

// IMAGE
ImageElement::ImageElement(ImagePtr image, MessageElementFlags flags)
    : MessageElement(flags)
    , image_(std::move(image))
{
}

void ImageElement::addToContainer(MessageLayoutContainer &container,
                                  const MessageLayoutContext &ctx)
{
    if (ctx.flags.hasAny(this->getFlags()))
    {
        container.addElement(new ImageLayoutElement(
            *this, this->image_, this->image_->size() * container.getScale()));
    }
}

std::unique_ptr<MessageElement> ImageElement::clone() const
{
    auto el = std::make_unique<ImageElement>(this->image_, this->getFlags());
    el->cloneFrom(*this);
    return el;
}

QJsonObject ImageElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"_L1] = u"ImageElement"_s;
    base["url"_L1] = this->image_->url().string;

    return base;
}

std::string_view ImageElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

CircularImageElement::CircularImageElement(ImagePtr image, int padding,
                                           QColor background,
                                           MessageElementFlags flags)
    : MessageElement(flags)
    , image_(std::move(image))
    , padding_(padding)
    , background_(background)
{
}

void CircularImageElement::addToContainer(MessageLayoutContainer &container,
                                          const MessageLayoutContext &ctx)
{
    if (ctx.flags.hasAny(this->getFlags()))
    {
        if (this->getFlags().has(MessageElementFlag::ReplyButton))
        {
            container.ensureSingleSpaceBeforeNextElement();
        }

        auto imgSize = QSize(this->image_->width(), this->image_->height()) *
                       container.getScale();

        container.addElement(new ImageWithCircleBackgroundLayoutElement(
            *this, this->image_, imgSize, this->background_, this->padding_));
    }
}

std::unique_ptr<MessageElement> CircularImageElement::clone() const
{
    auto el = std::make_unique<CircularImageElement>(
        this->image_, this->padding_, this->background_, this->getFlags());
    el->cloneFrom(*this);
    return el;
}

QJsonObject CircularImageElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"_L1] = u"CircularImageElement"_s;
    base["url"_L1] = this->image_->url().string;
    base["padding"_L1] = this->padding_;
    base["background"_L1] = this->background_.name(QColor::HexArgb);

    return base;
}

std::string_view CircularImageElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

// EMOTE
EmoteElement::EmoteElement(const EmotePtr &emote, MessageElementFlags flags,
                           const MessageColor &textElementColor)
    : MessageElement(flags)
    , textColor_(textElementColor)
    , emote_(emote)
{
    this->setTooltip(emote->tooltip.string);
}

EmotePtr EmoteElement::getEmote() const
{
    return this->emote_;
}

void EmoteElement::addToContainer(MessageLayoutContainer &container,
                                  const MessageLayoutContext &ctx)
{
    if (ctx.flags.hasNone(this->getFlags()))
    {
        return;
    }

    if (ctx.flags.has(MessageElementFlag::EmoteImage))
    {
        auto image =
            this->emote_->images.getImageOrLoaded(container.getImageScale());

        if (image->isEmpty())
        {
            this->ensureText(true);
        }
        else
        {
            bool isBadge = this->getFlags().hasAny(MessageElementFlag::Badges);
            auto scale = isBadge ? container.getBadgeScale()
                                 : container.getEmoteScale();
            auto emoteScale = getSettings()->emoteScale.getValue();
            auto size = image->size() * scale * emoteScale;

            container.addElement(this->makeImageLayoutElement(image, size));
            return;
        }
    }
    else
    {
        this->ensureText(false);
    }

    auto textCtx = ctx;
    textCtx.flags = MessageElementFlag::Misc;
    this->textElement_->addToContainer(container, textCtx);
}

MessageLayoutElement *EmoteElement::makeImageLayoutElement(
    const ImagePtr &image, QSizeF size)
{
    return new ImageLayoutElement(*this, image, size);
}

std::unique_ptr<MessageElement> EmoteElement::clone() const
{
    auto el = std::make_unique<EmoteElement>(this->emote_, this->getFlags(),
                                             this->textColor_);
    el->cloneFrom(*this);
    return el;
}

void EmoteElement::ensureText(bool asFallback)
{
    if (this->textElement_ && asFallback == this->usingFallbackColor_)
    {
        return;
    }

    auto color = this->textColor_;
    if (asFallback)
    {
        color = MessageColor::System;
    }
    this->textElement_ = std::make_unique<TextElement>(
        this->emote_->getCopyString(), MessageElementFlag::Misc, color);
    this->usingFallbackColor_ = asFallback;
}

QJsonObject EmoteElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"_L1] = u"EmoteElement"_s;
    base["emote"_L1] = this->emote_->toJson();
    if (this->textElement_)
    {
        base["text"_L1] = this->textElement_->toJson();
    }

    return base;
}

std::string_view EmoteElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

LayeredEmoteElement::LayeredEmoteElement(
    std::vector<LayeredEmoteElement::Emote> &&emotes, MessageElementFlags flags,
    const MessageColor &textElementColor)
    : MessageElement(flags)
    , emotes_(std::move(emotes))
    , textElementColor_(textElementColor)
{
    this->updateTooltips();
}

void LayeredEmoteElement::addEmoteLayer(const LayeredEmoteElement::Emote &emote)
{
    this->emotes_.push_back(emote);
    this->updateTooltips();
}

void LayeredEmoteElement::addToContainer(MessageLayoutContainer &container,
                                         const MessageLayoutContext &ctx)
{
    if (ctx.flags.hasAny(this->getFlags()))
    {
        if (ctx.flags.has(MessageElementFlag::EmoteImage))
        {
            auto images = this->getLoadedImages(container.getImageScale());
            if (images.empty())
            {
                return;
            }

            auto emoteScale = getSettings()->emoteScale.getValue();
            bool isBadge = this->getFlags().hasAny(MessageElementFlag::Badges);
            auto scale = isBadge ? container.getBadgeScale()
                                 : container.getEmoteScale();

            auto largestSize = getBoundingBoxSize(images) * scale * emoteScale;
            std::vector<QSizeF> individualSizes;
            individualSizes.reserve(this->emotes_.size());
            for (const auto &img : images)
            {
                individualSizes.push_back(img->size() * scale * emoteScale);
            }

            container.addElement(this->makeImageLayoutElement(
                images, individualSizes, largestSize));
        }
        else
        {
            if (this->textElement_)
            {
                auto textCtx = ctx;
                textCtx.flags = MessageElementFlag::Misc;
                this->textElement_->addToContainer(container, textCtx);
            }
        }
    }
}

std::vector<ImagePtr> LayeredEmoteElement::getLoadedImages(float scale)
{
    std::vector<ImagePtr> res;
    res.reserve(this->emotes_.size());

    for (const auto &emote : this->emotes_)
    {
        auto image = emote.ptr->images.getImageOrLoaded(scale);
        if (image->isEmpty())
        {
            continue;
        }
        res.push_back(image);
    }
    return res;
}

MessageLayoutElement *LayeredEmoteElement::makeImageLayoutElement(
    const std::vector<ImagePtr> &images, const std::vector<QSizeF> &sizes,
    QSizeF largestSize)
{
    return new LayeredImageLayoutElement(*this, images, sizes, largestSize);
}

void LayeredEmoteElement::updateTooltips()
{
    if (!this->emotes_.empty())
    {
        QString copyStr = this->getCopyString();
        this->textElement_.reset(new TextElement(
            copyStr, MessageElementFlag::Misc, this->textElementColor_));
        this->setTooltip(copyStr);
    }

    std::vector<QString> result;
    result.reserve(this->emotes_.size());

    for (const auto &emote : this->emotes_)
    {
        result.push_back(emote.ptr->tooltip.string);
    }

    this->emoteTooltips_ = std::move(result);
}

const std::vector<QString> &LayeredEmoteElement::getEmoteTooltips() const
{
    return this->emoteTooltips_;
}

QString LayeredEmoteElement::getCleanCopyString() const
{
    QString result;
    for (size_t i = 0; i < this->emotes_.size(); ++i)
    {
        if (i != 0)
        {
            result += " ";
        }
        result += TwitchEmotes::cleanUpEmoteCode(
            this->emotes_[i].ptr->getCopyString());
    }
    return result;
}

QString LayeredEmoteElement::getCopyString() const
{
    QString result;
    for (size_t i = 0; i < this->emotes_.size(); ++i)
    {
        if (i != 0)
        {
            result += " ";
        }
        result += this->emotes_[i].ptr->getCopyString();
    }
    return result;
}

const std::vector<LayeredEmoteElement::Emote> &LayeredEmoteElement::getEmotes()
    const
{
    return this->emotes_;
}

std::vector<LayeredEmoteElement::Emote> LayeredEmoteElement::getUniqueEmotes()
    const
{
    // Functor for std::copy_if that keeps track of seen elements
    struct NotDuplicate {
        bool operator()(const Emote &element)
        {
            return this->seen.insert(element.ptr).second;
        }

    private:
        std::set<EmotePtr> seen;
    };

    // Get unique emotes while maintaining relative layering order
    NotDuplicate dup;
    std::vector<Emote> unique;
    std::copy_if(this->emotes_.begin(), this->emotes_.end(),
                 std::back_insert_iterator(unique), dup);

    return unique;
}

const MessageColor &LayeredEmoteElement::textElementColor() const
{
    return this->textElementColor_;
}

std::unique_ptr<MessageElement> LayeredEmoteElement::clone() const
{
    auto emotes = this->getEmotes();
    auto el = std::make_unique<LayeredEmoteElement>(
        std::move(emotes), this->getFlags(), this->textElementColor());
    el->cloneFrom(*this);
    return el;
}

QJsonObject LayeredEmoteElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"_L1] = u"LayeredEmoteElement"_s;

    QJsonArray emotes;
    for (const auto &emote : this->emotes_)
    {
        emotes.append({{
            {"flags"_L1, qmagicenum::enumFlagsName(emote.flags.value())},
            {"emote"_L1, emote.ptr->toJson()},
        }});
    }
    base["emotes"_L1] = emotes;

    QJsonArray tooltips;
    for (const auto &tooltip : this->emoteTooltips_)
    {
        emotes.append(tooltip);
    }
    base["tooltips"_L1] = tooltips;

    if (this->textElement_)
    {
        base["text"_L1] = this->textElement_->toJson();
    }

    base["textElementColor"_L1] = this->textElementColor_.toString();

    return base;
}

std::string_view LayeredEmoteElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

// BADGE
BadgeElement::BadgeElement(const EmotePtr &emote, MessageElementFlags flags)
    : MessageElement(flags)
    , emote_(emote)
{
    this->setTooltip(emote->tooltip.string);
}

void BadgeElement::addToContainer(MessageLayoutContainer &container,
                                  const MessageLayoutContext &ctx)
{
    const auto elementFlags = this->getFlags();
    const auto rawFlags =
        static_cast<MessageElementFlags::Int>(elementFlags.value());
    constexpr auto HOMIES_SUPPORTER_FLAG =
        static_cast<MessageElementFlags::Int>(
            MessageElementFlag::BadgeHomiesSupporter);
    constexpr auto HOMIES_CUSTOM_FLAG =
        static_cast<MessageElementFlags::Int>(
            MessageElementFlag::BadgeHomiesCustom);

    const auto isHomiesSupporter =
        (rawFlags & HOMIES_SUPPORTER_FLAG) != 0;
    const auto isHomiesCustom = (rawFlags & HOMIES_CUSTOM_FLAG) != 0;
    const auto isHomiesBadge = isHomiesSupporter || isHomiesCustom;

    if (isHomiesBadge)
    {
        const auto *settings = getSettings();
        const auto enabled =
            ctx.flags.hasAny(elementFlags) &&
            ((isHomiesSupporter &&
              settings->showBadgesHomiesSupporter.getValue()) ||
             (isHomiesCustom &&
              settings->showBadgesHomiesCustom.getValue()));

        if (!enabled)
        {
            return;
        }
    }
    else if (!ctx.flags.hasAny(elementFlags))
    {
        return;
    }

    const auto image =
        isHomiesBadge ? this->emote_->images.getImageOrLoadedNoLoad(
                            container.getImageScale())
                      : this->emote_->images.getImageOrLoaded(
                            container.getImageScale());
    if (image->isEmpty())
    {
        return;
    }

    container.addElement(this->makeImageLayoutElement(
        image, image->size() * container.getScale()));
}

EmotePtr BadgeElement::getEmote() const
{
    return this->emote_;
}

MessageLayoutElement *BadgeElement::makeImageLayoutElement(
    const ImagePtr &image, QSizeF size)
{
    auto *element = new ImageLayoutElement(*this, image, size);

    return element;
}

std::unique_ptr<MessageElement> BadgeElement::clone() const
{
    auto el = std::make_unique<BadgeElement>(this->emote_, this->getFlags());
    el->cloneFrom(*this);
    return el;
}

QJsonObject BadgeElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"_L1] = u"BadgeElement"_s;
    base["emote"_L1] = this->emote_->toJson();

    return base;
}

std::string_view BadgeElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

// MOD BADGE
ModBadgeElement::ModBadgeElement(const EmotePtr &data,
                                 MessageElementFlags flags_)
    : BadgeElement(data, flags_)
{
}

MessageLayoutElement *ModBadgeElement::makeImageLayoutElement(
    const ImagePtr &image, QSizeF size)
{
    static const QColor modBadgeBackgroundColor("#34AE0A");

    auto *element = new ImageWithBackgroundLayoutElement(
        *this, image, size, modBadgeBackgroundColor);

    return element;
}

std::unique_ptr<MessageElement> ModBadgeElement::clone() const
{
    auto el = std::make_unique<ModBadgeElement>(this->emote_, this->getFlags());
    el->cloneFrom(*this);
    return el;
}

QJsonObject ModBadgeElement::toJson() const
{
    auto base = BadgeElement::toJson();
    base["type"_L1] = u"ModBadgeElement"_s;

    return base;
}

std::string_view ModBadgeElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

// VIP BADGE
VipBadgeElement::VipBadgeElement(const EmotePtr &data,
                                 MessageElementFlags flags_)
    : BadgeElement(data, flags_)
{
}

MessageLayoutElement *VipBadgeElement::makeImageLayoutElement(
    const ImagePtr &image, QSizeF size)
{
    auto *element = new ImageLayoutElement(*this, image, size);

    return element;
}

std::unique_ptr<MessageElement> VipBadgeElement::clone() const
{
    auto el = std::make_unique<VipBadgeElement>(this->emote_, this->getFlags());
    el->cloneFrom(*this);
    return el;
}

QJsonObject VipBadgeElement::toJson() const
{
    auto base = BadgeElement::toJson();
    base["type"_L1] = u"VipBadgeElement"_s;

    return base;
}

std::string_view VipBadgeElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

// FFZ Badge
FfzBadgeElement::FfzBadgeElement(const EmotePtr &data,
                                 MessageElementFlags flags_, QColor color_)
    : BadgeElement(data, flags_)
    , color(std::move(color_))
{
}

MessageLayoutElement *FfzBadgeElement::makeImageLayoutElement(
    const ImagePtr &image, QSizeF size)
{
    auto *element =
        new ImageWithBackgroundLayoutElement(*this, image, size, this->color);

    return element;
}

std::unique_ptr<MessageElement> FfzBadgeElement::clone() const
{
    auto el = std::make_unique<FfzBadgeElement>(this->emote_, this->getFlags(),
                                                this->color);
    el->cloneFrom(*this);
    return el;
}

QJsonObject FfzBadgeElement::toJson() const
{
    auto base = BadgeElement::toJson();
    base["type"_L1] = u"FfzBadgeElement"_s;
    base["color"_L1] = this->color.name(QColor::HexArgb);

    return base;
}

std::string_view FfzBadgeElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

// TEXT
TextElement::TextElement(const QString &text, MessageElementFlags flags,
                         const MessageColor &color, FontStyle style)
    : MessageElement(flags)
    , color_(color)
    , style_(style)
{
    this->words_ = text.split(' ');
    // fourtf: add logic to store multiple spaces after message
}

TextElement::TextElement(QStringList &&words, MessageElementFlags flags,
                         const MessageColor &color, FontStyle style)
    : MessageElement(flags)
    , words_(std::move(words))
    , color_(color)
    , style_(style)
{
}

void TextElement::addToContainer(MessageLayoutContainer &container,
                                 const MessageLayoutContext &ctx)
{
    auto *app = getApp();

    if (this->getFlags().has(MessageElementFlag::ChannelName) &&
        ctx.flags.hasAny(MessageElementFlag::PlatformBadgeAlways,
                         MessageElementFlag::PlatformBadgeIfUnselected) &&
        ctx.selectedChannel != nullptr)
    {
        EmotePtr emote;
        if (!ctx.flags.has(MessageElementFlag::PlatformBadgeIfUnselected) ||
            ctx.message.platform != ctx.selectedChannel->messagePlatform())
        {
            switch (ctx.message.platform)
            {
                case MessagePlatform::AnyOrTwitch:
                    emote = getTwitchBadge();
                    break;
                case MessagePlatform::Kick:
                    emote = getKickBadge();
                    break;
            }
        }

        if (emote)
        {
            auto image =
                emote->images.getImageOrLoaded(container.getImageScale());
            if (image->isEmpty())
            {
                return;
            }
            auto *el = new ImageLayoutElement(
                *this, image, image->size() * container.getScale());
            el->setLink(Link{});
            container.addElement(el);
        }
    }

    if (ctx.flags.hasAny(this->getFlags()))
    {
        if (this->getFlags().has(MessageElementFlag::RepeatedMessageCounter))
        {
            container.ensureSingleSpaceBeforeNextElement();
        }

        auto metrics =
            app->getFonts()->getFontMetrics(this->style_, container.getScale());
#ifdef Q_OS_WIN
        const bool measureUsernameWithLayout = false;
#else
        const bool measureUsernameWithLayout =
            this->getFlags().has(MessageElementFlag::Username);
#endif
        const auto usernameFont = measureUsernameWithLayout
                                      ? app->getFonts()->getFont(
                                            this->style_,
                                            container.getScale())
                                      : QFont{};
        const QFont *layoutFont =
            measureUsernameWithLayout ? &usernameFont : nullptr;

        for (const auto &word : this->words_)
        {
            auto wordId = container.nextWordId();

            auto getTextLayoutElement = [&](QString text, QSizeF size,
                                            bool hasTrailingSpace) {
                auto color = this->color_.getColor(ctx.messageColors);
                app->getThemes()->normalizeColor(color);

                auto *e = new TextLayoutElement(
                    *this, text, size, color,
                    this->style_, this->color_.type(), container.getScale(),
                    container.getImageScale() / container.getScale());
                e->setTrailingSpace(hasTrailingSpace);
                e->setText(text);
                e->setWordId(wordId);

                return e;
            };

            auto size = textElementSize(word, metrics, layoutFont);
            auto width = size.width();

            // see if the text fits in the current line
            if (container.fitsInLine(width))
            {
                container.addElementNoLineBreak(getTextLayoutElement(
                    word, size, this->hasTrailingSpace()));
                continue;
            }

            // see if the text fits in the next line
            if (!container.atStartOfLine())
            {
                container.breakLine();

                if (container.fitsInLine(width))
                {
                    container.addElementNoLineBreak(getTextLayoutElement(
                        word, size, this->hasTrailingSpace()));
                    continue;
                }
            }

            // We done goofed, we need to wrap the text.
            // If we allow the use of private Qt APIs, we can use Qt's text
            // engine to accurately calculate the width of the text. Otherwise,
            // we have to fall back to using horizontalAdvance which has some
            // corner cases when processing whole words (see #5944).
#ifdef CHATTERINO_WITH_PRIVATE_QT_API
            auto font =
                app->getFonts()->getFont(this->style_, container.getScale());

            // This code is similar to the one from QTextEngine::elidedText in
            // the mode Qt::ElideRight (because that's essentially what we're
            // doing here): https://github.com/qt/qtbase/blob/560bf5a07720eaa8cc589f424743db8ed1f1d902/src/gui/text/qtextengine.cpp#L3145
            // A difference is that, once we detected EOL, we start again.

            // The start of the current line in `word`
            qsizetype actualStart = 0;
            // This is treated like a view (from `actualStart`) over the word.
            // It's a QString because QStackTextEngine doesn't support
            // QStringViews as arguments.
            QString view = word;

            // This is essentially a loop over every line of text.
            do
            {
                QStackTextEngine engine(view, font);
                engine.validate();  // initialize the internal state

                int pos = 0;
                int nextBreak = 0;
                QFixed currentWidth = 0;
                int to = static_cast<int>(view.size());
                bool needsBreak = false;

                // Find the next grapheme boundary (`nextBreak`) at which we
                // need to break because the text wouldn't fit into the
                // container anymore.
                do
                {
                    pos = nextBreak;

                    ++nextBreak;
                    while (nextBreak < engine.layoutData->string.size() &&
                           !engine.attributes()[nextBreak].graphemeBoundary)
                    {
                        ++nextBreak;
                    }

                    auto nextWidth =
                        currentWidth + engine.width(pos, nextBreak - pos);
                    if (!container.fitsInLine(nextWidth.toReal()))
                    {
                        needsBreak = true;
                        if (pos == 0)
                        {
                            // Make sure that we consume at least one glyph.
                            // So this element will overflow
                            currentWidth = nextWidth;
                        }
                        else
                        {
                            // We didn't consume the glyph, it's for the next line
                            nextBreak = pos;
                        }
                        break;
                    }
                    currentWidth = nextWidth;
                } while (nextBreak < to);
                // Now we either processed the whole text or we need to break
                auto currentText = word.sliced(actualStart, nextBreak);
                auto currentSize =
                    textElementSize(currentText, metrics, layoutFont);
                if (layoutFont != nullptr)
                {
                    currentSize.setWidth(
                        std::max(currentSize.width(),
                                 std::ceil(currentWidth.toReal())));
                }
                else
                {
                    currentSize.setWidth(currentWidth.toReal());
                }
                container.addElementNoLineBreak(getTextLayoutElement(
                    currentText, currentSize,
                    !needsBreak && this->hasTrailingSpace()));
                if (needsBreak)
                {
                    container.breakLine();
                }

                actualStart += nextBreak;
                // Update the view
                view = QString::fromRawData(word.constData() + actualStart,
                                            word.size() - actualStart);
                assert(needsBreak || view.isEmpty());
            } while (!view.isEmpty());
#else
            auto textLength = word.length();
            int wordStart = 0;
            width = 0;

            for (int i = 0; i < textLength; i++)
            {
                auto isSurrogate = word.size() > i + 1 &&
                                   QChar::isHighSurrogate(word[i].unicode());

                auto charWidth = isSurrogate
                                     ? metrics.horizontalAdvance(word.mid(i, 2))
                                     : metrics.horizontalAdvance(word[i]);

                if (!container.fitsInLine(width + charWidth))
                {
                    auto currentText = word.mid(wordStart, i - wordStart);
                    auto currentSize =
                        textElementSize(currentText, metrics, layoutFont);
                    if (layoutFont != nullptr)
                    {
                        currentSize.setWidth(
                            std::max(currentSize.width(), std::ceil(width)));
                    }
                    else
                    {
                        currentSize.setWidth(width);
                    }
                    container.addElementNoLineBreak(getTextLayoutElement(
                        currentText, currentSize, false));
                    container.breakLine();

                    wordStart = i;
                    width = charWidth;

                    if (isSurrogate)
                    {
                        i++;
                    }
                    continue;
                }

                width += charWidth;

                if (isSurrogate)
                {
                    i++;
                }
            }
            //add the final piece of wrapped text
            auto currentText = word.mid(wordStart);
            auto currentSize = textElementSize(currentText, metrics, layoutFont);
            if (layoutFont != nullptr)
            {
                currentSize.setWidth(
                    std::max(currentSize.width(), std::ceil(width)));
            }
            else
            {
                currentSize.setWidth(width);
            }
            container.addElementNoLineBreak(getTextLayoutElement(
                currentText, currentSize, this->hasTrailingSpace()));
#endif
        }
    }
}

std::unique_ptr<MessageElement> TextElement::clone() const
{
    auto el = std::make_unique<TextElement>(QString(), this->getFlags(),
                                            this->color_, this->style_);
    el->words_ = this->words_;
    el->cloneFrom(*this);
    return el;
}

const MessageColor &TextElement::color() const noexcept
{
    return this->color_;
}

FontStyle TextElement::fontStyle() const noexcept
{
    return this->style_;
}

void TextElement::appendText(QStringView text)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    for (auto word : text.split(' '))  // creates a QList
#else
    for (auto word : text.tokenize(u' '))
#endif
    {
        this->words_.append(word.toString());
    }
}

void TextElement::appendText(const QString &text)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    this->appendText(QStringView{text});
#else
    qsizetype firstSpace = text.indexOf(u' ');
    if (firstSpace == -1)
    {
        // reuse (ref) `text`
        this->words_.emplace_back(text);
        return;
    }

    this->words_.emplace_back(text.sliced(0, firstSpace));
    for (auto word : QStringView{text}.sliced(firstSpace + 1).tokenize(u' '))
    {
        this->words_.emplace_back(word.toString());
    }
#endif
}

QJsonObject TextElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"_L1] = u"TextElement"_s;
    base["words"_L1] = QJsonArray::fromStringList(this->words_);
    base["color"_L1] = this->color_.toString();
    base["style"_L1] = qmagicenum::enumNameString(this->style_);

    return base;
}

std::string_view TextElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

SingleLineTextElement::SingleLineTextElement(const QString &text,
                                             MessageElementFlags flags,
                                             const MessageColor &color,
                                             FontStyle style)
    : MessageElement(flags)
    , color_(color)
    , style_(style)
    , words_(text.split(' '))
{
}

void SingleLineTextElement::addToContainer(MessageLayoutContainer &container,
                                           const MessageLayoutContext &ctx)
{
    auto *app = getApp();

    if (ctx.flags.hasAny(this->getFlags()))
    {
        auto metrics =
            app->getFonts()->getFontMetrics(this->style_, container.getScale());

        auto getTextLayoutElement = [&](QString text, qreal width,
                                        bool hasTrailingSpace) {
            auto color = this->color_.getColor(ctx.messageColors);
            app->getThemes()->normalizeColor(color);

            auto *e = new TextLayoutElement(
                *this, text, QSizeF(width, metrics.height()), color,
                this->style_, this->color_.type(), container.getScale());
            e->setTrailingSpace(hasTrailingSpace);
            e->setText(text);

            return e;
        };

        static const auto ellipsis = QStringLiteral("…");

        // String to continuously append words onto until we place it in the container
        // once we encounter an emote or reach the end of the message text. */
        QString currentText;

        bool firstIteration = true;
        for (const auto &word : this->words_)
        {
            if (firstIteration)
            {
                firstIteration = false;
            }
            else
            {
                currentText += ' ';
            }

            bool done = false;
            for (const auto &parsedWord :
                 app->getEmotes()->getEmojis()->parse(word))
            {
                if (std::holds_alternative<QStringView>(parsedWord))
                {
                    currentText += std::get<QStringView>(parsedWord);
                    QString prev =
                        currentText;  // only increments the ref-count
                    currentText =
                        metrics.elidedText(currentText, Qt::ElideRight,
                                           container.remainingWidth());
                    if (currentText != prev)
                    {
                        done = true;
                        break;
                    }
                }
                else if (std::holds_alternative<EmotePtr>(parsedWord))
                {
                    auto emote = std::get<EmotePtr>(parsedWord);
                    auto image =
                        emote->images.getImageOrLoaded(container.getScale());
                    if (!image->isEmpty())
                    {
                        auto emoteScale = getSettings()->emoteScale.getValue();

                        auto currentWidth =
                            metrics.horizontalAdvance(currentText);
                        auto emoteSize =
                            image->size() * emoteScale * container.getScale();

                        if (!container.fitsInLine(currentWidth +
                                                  emoteSize.width()))
                        {
                            currentText += ellipsis;
                            done = true;
                            break;
                        }

                        // Add currently pending text to container, then add the emote after.
                        container.addElementNoLineBreak(getTextLayoutElement(
                            currentText, currentWidth, false));
                        currentText.clear();

                        container.addElementNoLineBreak(
                            (new ImageLayoutElement(*this, image, emoteSize))
                                ->setLink(this->getLink())
                                ->setTrailingSpace(false));
                    }
                }
            }

            if (done)
            {
                break;
            }
        }

        // Add the last of the pending message text to the container.
        if (!currentText.isEmpty())
        {
            auto width = metrics.horizontalAdvance(currentText);
            container.addElementNoLineBreak(
                getTextLayoutElement(currentText, width, false));
        }

        container.breakLine();
    }
}

std::unique_ptr<MessageElement> SingleLineTextElement::clone() const
{
    auto el = std::make_unique<SingleLineTextElement>(
        QString(), this->getFlags(), this->color_, this->style_);
    el->words_ = this->words_;
    el->cloneFrom(*this);
    return el;
}

QJsonObject SingleLineTextElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"_L1] = u"SingleLineTextElement"_s;
    QJsonArray words = QJsonArray::fromStringList(this->words_);
    base["words"_L1] = words;
    base["color"_L1] = this->color_.toString();
    base["style"_L1] = qmagicenum::enumNameString(this->style_);

    return base;
}

std::string_view SingleLineTextElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

LinkElement::LinkElement(const Parsed &parsed, const QString &fullUrl,
                         MessageElementFlags flags, const MessageColor &color,
                         FontStyle style)
    : TextElement(QStringList(), flags, color, style)
    , linkInfo_(fullUrl)
    , lowercase_({parsed.lowercase})
    , original_({parsed.original})
{
    this->setTooltip(parsed.original);
}

void LinkElement::addToContainer(MessageLayoutContainer &container,
                                 const MessageLayoutContext &ctx)
{
    const auto &source =
        getSettings()->lowercaseDomains ? this->lowercase_ : this->original_;

    // Split the URL into segments at natural break points so long URLs
    // can wrap mid-link instead of being treated as a single unbreakable word.
    // Break points: before '/', '?', '&', '#', '='  (keep delimiter at start
    // of next segment so the visual break is clean).
    QStringList segments;
    for (const auto &url : source)
    {
        QString current;
        // Skip the protocol prefix (e.g. "https://") so we don't break there
        int start = 0;
        int protocolEnd = url.indexOf("://");
        if (protocolEnd != -1)
        {
            start = protocolEnd + 3;  // past "://"
            current = url.left(start);
        }

        for (int i = start; i < url.size(); i++)
        {
            QChar ch = url[i];
            // Break BEFORE these characters (except at the very start)
            if (!current.isEmpty() && current.size() > 1 &&
                (ch == '/' || ch == '?' || ch == '&' || ch == '#' ||
                 ch == '='))
            {
                segments.append(current);
                current.clear();
            }
            current += ch;
        }
        if (!current.isEmpty())
        {
            segments.append(current);
        }
    }

    this->words_ = segments;

    // Temporarily disable trailing space so URL segments render contiguously
    // (no visible gap between "example.com" and "/path").
    bool originalTrailingSpace = this->trailingSpace;
    this->trailingSpace = false;

    // Lay out all segments except the last with no trailing space
    if (!this->words_.isEmpty())
    {
        QStringList allButLast = this->words_.mid(0, this->words_.size() - 1);
        QString lastWord = this->words_.last();

        this->words_ = allButLast;
        TextElement::addToContainer(container, ctx);

        // Lay out the last segment with the original trailing space setting
        this->trailingSpace = originalTrailingSpace;
        this->words_ = {lastWord};
        TextElement::addToContainer(container, ctx);
    }

    // Restore words_ for any subsequent use (e.g. copy, tooltip)
    this->words_ = source;
    this->trailingSpace = originalTrailingSpace;
}

Link LinkElement::getLink() const
{
    return {Link::Url, this->linkInfo_.url()};
}

std::unique_ptr<MessageElement> LinkElement::clone() const
{
    auto el = std::make_unique<LinkElement>(
        Parsed{
            .lowercase = this->lowercase_.at(0),
            .original = this->original_.at(0),
        },
        this->linkInfo_.originalUrl(), this->getFlags(), this->color(),
        this->fontStyle());
    el->cloneFrom(*this);
    return el;
}

QJsonObject LinkElement::toJson() const
{
    auto base = TextElement::toJson();
    base["type"_L1] = u"LinkElement"_s;
    base["link"_L1] = this->linkInfo_.originalUrl();
    base["lowercase"_L1] = QJsonArray::fromStringList(this->lowercase_);
    base["original"_L1] = QJsonArray::fromStringList(this->original_);

    return base;
}

std::string_view LinkElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

MentionElement::MentionElement(const QString &displayName, QString loginName_,
                               MessageColor fallbackColor_,
                               MessageColor userColor_)
    : TextElement(displayName,
                  {MessageElementFlag::Text, MessageElementFlag::Mention})
    , fallbackColor_(fallbackColor_)
    , userColor_(userColor_)
    , userLoginName_(std::move(loginName_))
{
}

MentionElement::MentionElement(QStringList &&words, MessageColor fallbackColor_,
                               MessageColor userColor_)
    : TextElement(std::move(words),
                  {MessageElementFlag::Text, MessageElementFlag::Mention})
    , fallbackColor_(fallbackColor_)
    , userColor_(userColor_)
{
}

template <typename>
MentionElement::MentionElement(const QString &displayName, QString loginName_,
                               MessageColor fallbackColor_, QColor userColor_)
    : TextElement(displayName,
                  {MessageElementFlag::Text, MessageElementFlag::Mention})
    , fallbackColor_(fallbackColor_)
    , userColor_(userColor_.isValid() ? userColor_ : fallbackColor_)
    , userLoginName_(std::move(loginName_))
{
}

template MentionElement::MentionElement(const QString &displayName,
                                        QString loginName_,
                                        MessageColor fallbackColor_,
                                        QColor userColor_);

void MentionElement::addToContainer(MessageLayoutContainer &container,
                                    const MessageLayoutContext &ctx)
{
    if (getSettings()->colorUsernames)
    {
        this->color_ = this->userColor_;
    }
    else
    {
        this->color_ = this->fallbackColor_;
    }

    if (getSettings()->boldUsernames)
    {
        this->style_ = FontStyle::ChatMediumBold;
    }
    else
    {
        this->style_ = FontStyle::ChatMedium;
    }

    TextElement::addToContainer(container, ctx);
}

std::unique_ptr<MessageElement> MentionElement::clone() const
{
    std::unique_ptr<MentionElement> el{new MentionElement(
        this->words(), this->fallbackColor_, this->userColor_)};
    el->userLoginName_ = this->userLoginName_;
    el->cloneFrom(*this);
    return el;
}

MessageElement *MentionElement::setLink(const Link &link)
{
    assert(false && "MentionElement::setLink should not be called. Pass "
                    "through a valid login name in the constructor and it will "
                    "automatically be a UserInfo link");

    return TextElement::setLink(link);
}

Link MentionElement::getLink() const
{
    if (this->userLoginName_.isEmpty())
    {
        // Some rare mention elements don't have the knowledge of the login name
        return {};
    }

    return {Link::UserInfo, this->userLoginName_};
}

QJsonObject MentionElement::toJson() const
{
    auto base = TextElement::toJson();
    base["type"_L1] = u"MentionElement"_s;
    base["fallbackColor"_L1] = this->fallbackColor_.toString();
    base["userColor"_L1] = this->userColor_.toString();
    base["userLoginName"_L1] = this->userLoginName_;

    return base;
}

std::string_view MentionElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

// TIMESTAMP
TimestampElement::TimestampElement()
    : TimestampElement(getApp()->isTest() ? QTime::fromMSecsSinceStartOfDay(0)
                                          : QTime::currentTime())
{
}

TimestampElement::TimestampElement(QTime time)
    : MessageElement(MessageElementFlag::Timestamp)
    , time_(time)
    , element_(this->formatTime(time))
{
    assert(this->element_ != nullptr);
}

void TimestampElement::addToContainer(MessageLayoutContainer &container,
                                      const MessageLayoutContext &ctx)
{
    if (ctx.flags.hasAny(this->getFlags()))
    {
        this->setTooltip(this->getTooltip());
        if (getSettings()->timestampFormat != this->format_)
        {
            this->format_ = getSettings()->timestampFormat.getValue();
            this->element_.reset(this->formatTime(this->time_));
        }

        this->element_->addToContainer(container, ctx);
    }
}

TextElement *TimestampElement::formatTime(const QTime &time)
{
    static QLocale locale("en_US");

    QString format = locale.toString(time, getSettings()->timestampFormat);

    auto *text =
        new TextElement(format, MessageElementFlag::Timestamp,
                        MessageColor::System, FontStyle::TimestampMedium);
    text->setLink(this->getLink());
    text->setTooltip(this->getTooltip());
    return text;
}

MessageElement *TimestampElement::setLink(const Link &link)
{
    MessageElement::setLink(link);
    this->element_->setLink(link);
    return this;
}

std::unique_ptr<MessageElement> TimestampElement::clone() const
{
    auto el = std::make_unique<TimestampElement>(this->time_);
    el->cloneFrom(*this);
    return el;
}

QJsonObject TimestampElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"_L1] = u"TimestampElement"_s;
    base["time"_L1] = this->time_.toString(Qt::ISODate);
    base["element"_L1] = this->element_->toJson();
    base["format"_L1] = this->format_;

    return base;
}

std::string_view TimestampElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

// TWITCH MODERATION
namespace {

int normalizeInlineActionMode(int mode)
{
    if (mode <= 0)
    {
        return 0;
    }
    if (mode >= 2)
    {
        return 2;
    }
    return 1;
}

void addModerationActionToContainer(MessageElement &source,
                                    const ModerationAction &action,
                                    MessageLayoutContainer &container,
                                    const QSizeF &size)
{
    if (const auto &image = action.getImage())
    {
        container.addElement(
            (new ImageLayoutElement(source, *image, size))
                ->setLink(Link(Link::UserAction, action.getAction())));
    }
    else
    {
        container.addElement(
            (new TextIconLayoutElement(source, action.getLine1(),
                                       action.getLine2(),
                                       container.getScale(), size))
                ->setLink(Link(Link::UserAction, action.getAction())));
    }
}

}  // namespace

TwitchModerationElement::TwitchModerationElement(
    bool canModerateUser, bool targetIsModOrBroadcaster,
    bool targetIsCurrentUser)
    : MessageElement(MessageElementFlag::ModeratorTools)
    , canModerateUser_(canModerateUser)
    , targetIsModOrBroadcaster_(targetIsModOrBroadcaster)
    , targetIsCurrentUser_(targetIsCurrentUser)
{
}

void TwitchModerationElement::addToContainer(MessageLayoutContainer &container,
                                             const MessageLayoutContext &ctx)
{
    const bool inModerationMode =
        ctx.flags.has(MessageElementFlag::ModeratorTools);
    auto *settings = getSettings();
    const auto selfDeleteMode =
        normalizeInlineActionMode(settings->showSelfDeleteButton.getValue());
    const auto pinOnModeratorsMode =
        normalizeInlineActionMode(
            settings->showPinButtonOnModeratorsMode.getValue());
    const bool showSelfDeleteOutsideModerationMode =
        this->targetIsCurrentUser_ && selfDeleteMode == 2;
    const bool showPinOutsideModerationMode =
        this->targetIsModOrBroadcaster_ && pinOnModeratorsMode == 2;
    if (!inModerationMode && !showSelfDeleteOutsideModerationMode &&
        !showPinOutsideModerationMode)
    {
        return;
    }

    QSizeF size{
        container.getScale() * 16,
        container.getScale() * 16,
    };

    bool hasVisiblePinAction = false;
    bool hasVisibleDeleteAction = false;
    auto actions = settings->moderationActions.readOnly();
    for (const auto &action : *actions)
    {
        if (!this->shouldShowAction(action, inModerationMode, selfDeleteMode,
                                    pinOnModeratorsMode))
        {
            continue;
        }

        switch (action.getType())
        {
            case ModerationAction::Type::Pin:
                hasVisiblePinAction = true;
                break;
            case ModerationAction::Type::Delete:
                hasVisibleDeleteAction = true;
                break;
            default:
                break;
        }

        addModerationActionToContainer(*this, action, container, size);
    }

    auto addBuiltInAction = [&](const QString &command) {
        const ModerationAction action(command);
        addModerationActionToContainer(*this, action, container, size);
    };

    if (!hasVisiblePinAction && this->targetIsModOrBroadcaster_ &&
        pinOnModeratorsMode != 0 &&
        (inModerationMode || pinOnModeratorsMode == 2))
    {
        addBuiltInAction("/pin {msg.id}");
    }

    if (!hasVisibleDeleteAction && this->targetIsCurrentUser_ &&
        selfDeleteMode != 0 && (inModerationMode || selfDeleteMode == 2))
    {
        addBuiltInAction("/delete {msg.id}");
    }
}

std::unique_ptr<MessageElement> TwitchModerationElement::clone() const
{
    auto el = std::make_unique<TwitchModerationElement>(
        this->canModerateUser_, this->targetIsModOrBroadcaster_,
        this->targetIsCurrentUser_);
    el->cloneFrom(*this);
    return el;
}

QJsonObject TwitchModerationElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"_L1] = u"TwitchModerationElement"_s;

    return base;
}

std::string_view TwitchModerationElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

bool TwitchModerationElement::shouldShowAction(
    const ModerationAction &action, bool inModerationMode, int selfDeleteMode,
    int pinOnModeratorsMode) const
{
    if (!inModerationMode &&
        action.getType() != ModerationAction::Type::Delete &&
        action.getType() != ModerationAction::Type::Pin)
    {
        return false;
    }

    switch (action.getType())
    {
        case ModerationAction::Type::Delete:
            if (this->targetIsCurrentUser_)
            {
                return selfDeleteMode != 0 &&
                       (inModerationMode || selfDeleteMode == 2);
            }
            return this->canModerateUser_ && inModerationMode;
        case ModerationAction::Type::Pin:
            if (this->targetIsModOrBroadcaster_)
            {
                return pinOnModeratorsMode != 0 &&
                       (inModerationMode || pinOnModeratorsMode == 2);
            }
            return this->canModerateUser_ && inModerationMode;
        case ModerationAction::Type::Ban:
        case ModerationAction::Type::Timeout:
        case ModerationAction::Type::Custom:
            return this->canModerateUser_;
    }

    return false;
}

LinebreakElement::LinebreakElement(MessageElementFlags flags)
    : MessageElement(flags)
{
}

void LinebreakElement::addToContainer(MessageLayoutContainer &container,
                                      const MessageLayoutContext &ctx)
{
    if (ctx.flags.hasAny(this->getFlags()))
    {
        container.breakLine();
    }
}

std::unique_ptr<MessageElement> LinebreakElement::clone() const
{
    auto el = std::make_unique<LinebreakElement>(this->getFlags());
    el->cloneFrom(*this);
    return el;
}

QJsonObject LinebreakElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"_L1] = u"LinebreakElement"_s;

    return base;
}

std::string_view LinebreakElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

ScalingImageElement::ScalingImageElement(ImageSet images,
                                         MessageElementFlags flags)
    : MessageElement(flags)
    , images_(std::move(images))
{
}

void ScalingImageElement::addToContainer(MessageLayoutContainer &container,
                                         const MessageLayoutContext &ctx)
{
    if (ctx.flags.hasAny(this->getFlags()))
    {
        const auto &image =
            this->images_.getImageOrLoaded(container.getImageScale());
        if (image->isEmpty())
        {
            return;
        }

        container.addElement(new ImageLayoutElement(
            *this, image, image->size() * container.getScale()));
    }
}

std::unique_ptr<MessageElement> ScalingImageElement::clone() const
{
    auto el =
        std::make_unique<ScalingImageElement>(this->images_, this->getFlags());
    el->cloneFrom(*this);
    return el;
}

QJsonObject ScalingImageElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"_L1] = u"ScalingImageElement"_s;
    base["image"_L1] = this->images_.getImage1()->url().string;

    return base;
}

std::string_view ScalingImageElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

ReplyCurveElement::ReplyCurveElement()
    : MessageElement(MessageElementFlag::RepliedMessage)
{
}

void ReplyCurveElement::addToContainer(MessageLayoutContainer &container,
                                       const MessageLayoutContext &ctx)
{
    static const qreal width = 18;       // Overall width
    static const float thickness = 1.5;  // Pen width
    static const int radius = 6;         // Radius of the top left corner
    static const int margin = 2;         // Top/Left/Bottom margin

    if (ctx.flags.hasAny(this->getFlags()))
    {
        float scale = container.getScale();
        container.addElement(
            new ReplyCurveLayoutElement(*this, width * scale, thickness * scale,
                                        radius * scale, margin * scale));
    }
}

std::unique_ptr<MessageElement> ReplyCurveElement::clone() const
{
    auto el = std::make_unique<ReplyCurveElement>();
    el->cloneFrom(*this);
    return el;
}

QJsonObject ReplyCurveElement::toJson() const
{
    auto base = MessageElement::toJson();
    base["type"_L1] = u"ReplyCurveElement"_s;

    return base;
}

std::string_view ReplyCurveElement::type() const
{
    return std::remove_pointer_t<decltype(this)>::TYPE;
}

}  // namespace chatterino

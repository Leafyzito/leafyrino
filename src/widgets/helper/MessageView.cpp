// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/MessageView.hpp"

#include "Application.hpp"
#include "common/Common.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/layouts/MessageLayout.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/MessageElement.hpp"
#include "messages/Selection.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/links/LinkInfo.hpp"
#include "providers/links/LinkResolver.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Clipboard.hpp"
#include "util/DistanceBetweenPoints.hpp"
#include "util/IncognitoBrowser.hpp"
#include "util/Twitch.hpp"
#include "widgets/dialogs/SettingsDialog.hpp"
#include "widgets/dialogs/UserInfoPopup.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/TooltipWidget.hpp"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QShortcut>
#include <QUrl>

#include <climits>

namespace {

using namespace chatterino;

constexpr size_t TOOLTIP_EMOTE_ENTRIES_LIMIT = 7;

const MessageElementFlags MESSAGE_FLAGS{
    MessageElementFlag::Text,
    MessageElementFlag::EmojiAll,
    MessageElementFlag::EmoteText,
};

float getTooltipScale(EmoteTooltipScale emoteTooltipScale)
{
    switch (emoteTooltipScale)
    {
        case EmoteTooltipScale::Small:
            return 0.5F;
        case EmoteTooltipScale::Medium:
            return 1.0F;
        case EmoteTooltipScale::Large:
            return 1.5F;
        case EmoteTooltipScale::Huge:
            return 2.0F;

        default:
            return 1.0F;
    }
}

Split *findAncestorSplit(QWidget *widget)
{
    for (auto *w = widget; w != nullptr; w = w->parentWidget())
    {
        if (auto *split = qobject_cast<Split *>(w))
        {
            return split;
        }
    }
    return nullptr;
}

}  // namespace

namespace chatterino {

MessageView::MessageView(QWidget *parent)
    : BaseWidget(parent)
    , tooltipWidget_(new TooltipWidget(this))
{
    this->setMouseTracking(true);

    this->clickTimer_.setSingleShot(true);
    this->clickTimer_.setInterval(500);

    auto *copyShortcut = new QShortcut(QKeySequence::StandardKey::Copy, this);
    QObject::connect(copyShortcut, &QShortcut::activated, this, [this] {
        this->copySelectedText();
    });

    this->messagePreferences_.connectSettings(getSettings(),
                                              this->signalHolder_);

    this->signalHolder_.managedConnect(
        getApp()->getWindows()->gifRepaintRequested, [this] {
            if (this->hasAnimatedElements_ && this->isVisible())
            {
                this->update();
            }
        });
}

MessageView::~MessageView() = default;

void MessageView::createMessageLayout()
{
    if (this->message_ == nullptr)
    {
        this->messageLayout_.reset();
        return;
    }

    this->messageLayout_ = std::make_unique<MessageLayout>(this->message_);
}

void MessageView::setMessage(const MessagePtr &message)
{
    this->layoutUsesChatWordFlags_ = false;
    this->clearSelection();

    if (!message)
    {
        this->message_.reset();
        this->messageLayout_.reset();
        this->hasAnimatedElements_ = false;
        this->tooltipWidget_->hide();
        this->update();
        return;
    }

    auto singleLineMessage = std::make_shared<Message>();
    singleLineMessage->elements.emplace_back(
        std::make_unique<SingleLineTextElement>(
            message->messageText, MESSAGE_FLAGS, MessageColor::Type::System,
            FontStyle::ChatMediumSmall));
    this->message_ = std::move(singleLineMessage);
    this->createMessageLayout();
    this->layoutMessage();
}

void MessageView::setFullMessage(const MessagePtr &message)
{
    this->layoutUsesChatWordFlags_ = true;

    const bool keepSelection = message && this->message_ &&
                               !message->id.isEmpty() &&
                               message->id == this->message_->id;
    if (!keepSelection)
    {
        this->clearSelection();
    }

    if (!message)
    {
        this->message_.reset();
        this->messageLayout_.reset();
        this->hasAnimatedElements_ = false;
        this->tooltipWidget_->hide();
        this->update();
        return;
    }

    this->message_ = message;
    this->createMessageLayout();
    this->layoutMessage();
}

void MessageView::clearMessage()
{
    this->setMessage(nullptr);
}

void MessageView::setWidth(int width)
{
    if (this->width_ != width)
    {
        this->width_ = width;
        this->layoutMessage();
    }
}

void MessageView::relayout()
{
    this->layoutMessage();
}

void MessageView::setLinkInfoTooltip(LinkInfo *info)
{
    assert(info);

    const auto thumbnailSize = getSettings()->thumbnailSize;

    ImagePtr thumbnail;
    if (info->hasThumbnail() && thumbnailSize > 0)
    {
        if (getApp()->getStreamerMode()->isEnabled() &&
            getSettings()->streamerModeHideLinkThumbnails)
        {
            thumbnail = Image::fromResourcePixmap(getResources().streamerMode);
        }
        else
        {
            thumbnail = info->thumbnail();
        }
    }

    this->tooltipWidget_->setOne({
        .image = thumbnail,
        .text = info->tooltip(),
        .customWidth = thumbnailSize,
        .customHeight = thumbnailSize,
    });

    if (info->isLoaded())
    {
        this->pendingLinkInfo_.clear();
        return;
    }

    if (this->pendingLinkInfo_.data() == info)
    {
        return;
    }

    if (this->pendingLinkInfo_)
    {
        QObject::disconnect(this->pendingLinkInfo_.data(),
                            &LinkInfo::stateChanged, this, nullptr);
    }
    QObject::connect(info, &LinkInfo::stateChanged, this,
                     &MessageView::pendingLinkInfoStateChanged);
    this->pendingLinkInfo_ = info;
}

void MessageView::pendingLinkInfoStateChanged()
{
    if (!this->pendingLinkInfo_)
    {
        return;
    }
    this->setLinkInfoTooltip(this->pendingLinkInfo_.data());
    this->tooltipWidget_->applyLastBoundsCheck();
}

void MessageView::updateHoverTooltip(QMouseEvent *event)
{
    if (this->messageLayout_ == nullptr)
    {
        return;
    }

    if (this->messageLayout_->flags.has(MessageLayoutFlag::Collapsed))
    {
        this->setCursor(Qt::PointingHandCursor);
        this->tooltipWidget_->hide();
        return;
    }

    const auto *hoverLayoutElement =
        this->messageLayout_->getElementAt(event->pos());

    if (hoverLayoutElement == nullptr)
    {
        this->setCursor(Qt::ArrowCursor);
        this->tooltipWidget_->hide();
        return;
    }

    auto *element = &hoverLayoutElement->getCreator();
    const bool isLinkValid = hoverLayoutElement->getLink().isValid();
    const auto *emoteElement = dynamic_cast<const EmoteElement *>(element);
    const auto *emoteLinkElement =
        dynamic_cast<const EmoteLinkElement *>(element);
    const auto *layeredEmoteElement =
        dynamic_cast<const LayeredEmoteElement *>(element);
    const bool isNotEmote =
        emoteElement == nullptr && emoteLinkElement == nullptr &&
        layeredEmoteElement == nullptr;

    if (element->getTooltip().isEmpty() ||
        (isLinkValid && isNotEmote && !getSettings()->linkInfoTooltip))
    {
        this->tooltipWidget_->hide();
    }
    else
    {
        const auto *badgeElement = dynamic_cast<const BadgeElement *>(element);

        if (badgeElement || emoteElement || emoteLinkElement ||
            layeredEmoteElement)
        {
            const auto showThumbnailSetting =
                getSettings()->emotesTooltipPreview.getEnum();

            const bool showThumbnail =
                showThumbnailSetting == ThumbnailPreviewMode::AlwaysShow ||
                (showThumbnailSetting == ThumbnailPreviewMode::ShowOnShift &&
                 event->modifiers() == Qt::ShiftModifier);

            if (emoteElement || emoteLinkElement)
            {
                const auto emote = emoteElement ? emoteElement->getEmote()
                                                : emoteLinkElement->getEmote();
                const auto scale = getSettings()->emoteTooltipScale.getEnum();
                this->tooltipWidget_->setOne(TooltipEntry::scaled(
                    showThumbnail ? emote->images.getImage(3.0) : nullptr,
                    element->getTooltip(), getTooltipScale(scale)));
            }
            else if (layeredEmoteElement)
            {
                const auto &layeredEmotes = layeredEmoteElement->getEmotes();
                if (!layeredEmotes.empty())
                {
                    std::vector<TooltipEntry> entries;
                    entries.reserve(layeredEmotes.size());

                    const auto &emoteTooltips =
                        layeredEmoteElement->getEmoteTooltips();

                    bool truncating = false;
                    size_t upperLimit = layeredEmotes.size();
                    if (layeredEmotes.size() > TOOLTIP_EMOTE_ENTRIES_LIMIT)
                    {
                        upperLimit = TOOLTIP_EMOTE_ENTRIES_LIMIT - 1;
                        truncating = true;
                    }

                    for (size_t i = 0; i < upperLimit; ++i)
                    {
                        const auto &emote = layeredEmotes[i].ptr;
                        if (i == 0)
                        {
                            const auto scale =
                                getSettings()->emoteTooltipScale.getEnum();
                            entries.push_back(TooltipEntry::scaled(
                                showThumbnail ? emote->images.getImage(3.0)
                                              : nullptr,
                                emoteTooltips[i], getTooltipScale(scale)));
                        }
                        else
                        {
                            const auto scale =
                                getSettings()->emoteTooltipScale.getEnum();
                            entries.push_back(TooltipEntry::scaled(
                                showThumbnail ? emote->images.getImage(1.0)
                                              : nullptr,
                                emote->name.string, getTooltipScale(scale)));
                        }
                    }

                    if (truncating)
                    {
                        entries.push_back({nullptr, "..."});
                    }

                    const auto style = layeredEmotes.size() > 2
                                           ? TooltipStyle::Grid
                                           : TooltipStyle::Vertical;
                    this->tooltipWidget_->set(entries, style);
                }
            }
            else if (badgeElement)
            {
                const auto scale = getSettings()->emoteTooltipScale.getEnum();
                this->tooltipWidget_->setOne(TooltipEntry::scaled(
                    showThumbnail
                        ? badgeElement->getEmote()->images.getImage(3.0)
                        : nullptr,
                    element->getTooltip(), getTooltipScale(scale)));
            }
        }
        else if (auto *linkElement = dynamic_cast<LinkElement *>(element))
        {
            if (linkElement->linkInfo()->isPending())
            {
                getApp()->getLinkResolver()->resolve(linkElement->linkInfo());
            }
            this->setLinkInfoTooltip(linkElement->linkInfo());
        }
        else
        {
            this->tooltipWidget_->setOne(TooltipEntry{
                .image = nullptr,
                .text = element->getTooltip(),
            });
        }

        this->tooltipWidget_->moveTo(
            event->globalPosition().toPoint() + QPoint(16, 16),
            widgets::BoundsChecking::CursorPosition);
        this->tooltipWidget_->setWordWrap(isLinkValid);
        this->tooltipWidget_->show();
    }

    if (isLinkValid)
    {
        this->setCursor(Qt::PointingHandCursor);
    }
    else
    {
        this->setCursor(Qt::ArrowCursor);
    }
}

void MessageView::mouseMoveEvent(QMouseEvent *event)
{
    if (this->messageLayout_ != nullptr &&
        !this->messageLayout_->flags.has(MessageLayoutFlag::Collapsed))
    {
        if (this->isLeftMouseDown_)
        {
            const size_t index =
                this->messageLayout_->getSelectionIndex(QPointF(event->pos()));
            this->setSelection(this->selection_.start, SelectionItem(0, index));
        }
        if (this->isDoubleClick_)
        {
            const auto *hoverLayoutElement =
                this->messageLayout_->getElementAt(event->pos());
            if (hoverLayoutElement != nullptr)
            {
                auto [wordStart, wordEnd] = this->messageLayout_->getWordBounds(
                    hoverLayoutElement, event->pos());
                const Selection hoveredWord{
                    SelectionItem(0, static_cast<size_t>(wordStart)),
                    SelectionItem(0, static_cast<size_t>(wordEnd))};
                const Selection selectUnion =
                    this->doubleClickSelection_ | hoveredWord;
                this->setSelection(selectUnion);
            }
        }
    }

    BaseWidget::mouseMoveEvent(event);
    this->updateHoverTooltip(event);
}

void MessageView::leaveEvent(QEvent *event)
{
    this->tooltipWidget_->hide();
    BaseWidget::leaveEvent(event);
}

void MessageView::showUserInfoPopup(const QString &userName)
{
    if (this->message_ == nullptr)
    {
        return;
    }

    auto *split = findAncestorSplit(this);
    if (split == nullptr)
    {
        openTwitchUsercard(this->message_->channelName, userName);
        return;
    }

    auto *userPopup =
        new UserInfoPopup(getSettings()->autoCloseUserPopup, split);

    auto openingChannel = split->getChannel();
    ChannelPtr contextChannel;
    if (openingChannel && openingChannel->isKickChannel())
    {
        contextChannel = getApp()->getKickChatServer()->findBySlug(
            this->message_->channelName);
        if (!contextChannel)
        {
            contextChannel = Channel::getEmpty();
        }
    }
    else
    {
        contextChannel = getApp()->getTwitch()->getChannelOrEmpty(
            this->message_->channelName);
    }
    userPopup->setData(userName, contextChannel, openingChannel);

    QPoint offset(userPopup->width() / 3, userPopup->height() / 5);
    userPopup->moveTo(QCursor::pos() - offset,
                      widgets::BoundsChecking::CursorPosition);
    userPopup->show();
}

void MessageView::handleLinkClick(QMouseEvent *event, const Link &link)
{
    if (event->button() != Qt::LeftButton &&
        event->button() != Qt::MiddleButton)
    {
        return;
    }

    switch (link.type)
    {
        case Link::UserWhisper:
        case Link::UserInfo:
            this->showUserInfoPopup(link.value);
            break;

        case Link::Url:
            if (getSettings()->openLinksIncognito && supportsIncognitoLinks())
            {
                openLinkIncognito(link.value);
            }
            else
            {
                QDesktopServices::openUrl(QUrl(link.value));
            }
            break;

        case Link::OpenAccountsPage:
            SettingsDialog::showDialog(this,
                                       SettingsDialogPreference::Accounts);
            break;

        case Link::CopyToClipboard:
            crossPlatformCopy(link.value);
            break;

        default:
            break;
    }
}

void MessageView::handleMouseClickFromRelease(
    QMouseEvent *event, const MessageLayoutElement *hoveredElement)
{
    if (event->button() != Qt::LeftButton &&
        event->button() != Qt::MiddleButton)
    {
        return;
    }
    if (hoveredElement == nullptr)
    {
        return;
    }

    const auto &link = hoveredElement->getLink();
    if (!getSettings()->linksDoubleClickOnly.getValue())
    {
        this->handleLinkClick(event, link);
    }

    if (link.type == Link::InsertText)
    {
        if (auto *split = findAncestorSplit(this))
        {
            split->insertTextToInput(link.value);
        }
    }
}

void MessageView::setSelection(const Selection &newSelection)
{
    if (this->selection_ != newSelection)
    {
        this->selection_ = newSelection;
        this->selectionChanged.invoke();
        this->update();
    }
}

void MessageView::setSelection(const SelectionItem &start,
                               const SelectionItem &end)
{
    this->setSelection({start, end});
}

void MessageView::selectWholeMessage()
{
    if (this->messageLayout_ == nullptr)
    {
        return;
    }
    const SelectionItem msgStart(
        0, this->messageLayout_->getFirstMessageCharacterIndex());
    const SelectionItem msgEnd(0,
                               this->messageLayout_->getLastCharacterIndex());
    this->setSelection(msgStart, msgEnd);
}

QString MessageView::getSelectedText() const
{
    if (this->messageLayout_ == nullptr || this->selection_.isEmpty())
    {
        return {};
    }

    QString result;
    this->messageLayout_->addSelectionText(
        result, static_cast<uint32_t>(this->selection_.selectionMin.charIndex),
        static_cast<uint32_t>(this->selection_.selectionMax.charIndex + 1));
    return result;
}

bool MessageView::hasSelection() const
{
    return !this->selection_.isEmpty();
}

void MessageView::copySelectedText()
{
    crossPlatformCopy(this->getSelectedText());
}

void MessageView::clearSelection()
{
    this->setSelection(Selection());
}

void MessageView::contextMenuEvent(QContextMenuEvent *event)
{
    if (this->messageLayout_ == nullptr)
    {
        BaseWidget::contextMenuEvent(event);
        return;
    }

    QMenu menu;
    if (!this->selection_.isEmpty())
    {
        menu.addAction(QStringLiteral("&Copy selection"), [this] {
            this->copySelectedText();
        });
    }

    menu.addAction(QStringLiteral("Copy &message"), [this] {
        QString copyString;
        this->messageLayout_->addSelectionText(copyString, 0, UINT32_MAX,
                                               CopyMode::OnlyTextAndEmotes);
        crossPlatformCopy(copyString);
    });

    menu.addAction(QStringLiteral("Copy &full message"), [this] {
        QString copyString;
        this->messageLayout_->addSelectionText(copyString, 0, UINT32_MAX,
                                               CopyMode::EverythingButReplies);
        crossPlatformCopy(copyString);
    });

    if (!this->selection_.isEmpty() && getSettings()->searchEnabled.getValue())
    {
        const QString searchURL = getSettings()->searchEngineUrl.getValue();
        const QString searchName = getSettings()->searchEngineName.getValue();

        if (!searchURL.isEmpty())
        {
            QString actionText =
                searchName.isEmpty()
                    ? QStringLiteral("&Search")
                    : QStringLiteral("&Search with %1").arg(searchName);

            if (getSettings()->searchIncognito && supportsIncognitoLinks())
            {
                actionText += QStringLiteral(" in private mode");
            }

            menu.addAction(actionText, [this, searchURL] {
                QString query = this->getSelectedText().trimmed();
                QString encodedQuery = QUrl::toPercentEncoding(query);
                QString url = searchURL + encodedQuery;

                if (getSettings()->searchIncognito && supportsIncognitoLinks())
                {
                    openLinkIncognito(url);
                }
                else
                {
                    QDesktopServices::openUrl(QUrl(url));
                }
            });
        }
    }

    menu.exec(event->globalPos());
}

void MessageView::mousePressEvent(QMouseEvent *event)
{
    if (this->messageLayout_ == nullptr)
    {
        BaseWidget::mousePressEvent(event);
        return;
    }

    if (this->messageLayout_->flags.has(MessageLayoutFlag::Collapsed))
    {
        if (event->button() == Qt::LeftButton)
        {
            this->messageLayout_->flags.set(MessageLayoutFlag::Expanded);
            this->messageLayout_->flags.set(MessageLayoutFlag::RequiresLayout);
            this->layoutMessage();
            event->accept();
            return;
        }
    }

    switch (event->button())
    {
        case Qt::LeftButton: {
            this->lastLeftPressPosition_ = event->globalPosition();
            this->isLeftMouseDown_ = true;

            const size_t index =
                this->messageLayout_->getSelectionIndex(QPointF(event->pos()));
            const SelectionItem item(0, index);
            this->setSelection(item, item);
        }
        break;

        default:
            break;
    }

    this->update();
}

void MessageView::mouseReleaseEvent(QMouseEvent *event)
{
    if (this->messageLayout_ == nullptr)
    {
        BaseWidget::mouseReleaseEvent(event);
        return;
    }

    this->layoutMessage();

    if (event->button() == Qt::LeftButton)
    {
        if (this->isDoubleClick_)
        {
            this->isDoubleClick_ = false;
            if (std::abs(distanceBetweenPoints(this->lastDoubleClickPosition_,
                                               event->globalPosition())) > 10.F)
            {
                this->clickTimer_.stop();
                return;
            }
        }
        else if (this->isLeftMouseDown_)
        {
            this->isLeftMouseDown_ = false;

            if (std::abs(distanceBetweenPoints(this->lastLeftPressPosition_,
                                               event->globalPosition())) > 15.F)
            {
                return;
            }

            if (this->clickTimer_.isActive() &&
                (std::abs(distanceBetweenPoints(this->lastDoubleClickPosition_,
                                                event->globalPosition())) <
                 10.F))
            {
                this->selectWholeMessage();
                return;
            }
        }
        else
        {
            return;
        }
    }
    else if (event->button() == Qt::MiddleButton)
    {
        const auto *hoverElement =
            this->messageLayout_->getElementAt(event->pos());
        if (hoverElement != nullptr && hoverElement->getLink().isValid() &&
            !getSettings()->linksDoubleClickOnly.getValue())
        {
            this->handleLinkClick(event, hoverElement->getLink());
        }
        this->update();
        return;
    }
    else
    {
        BaseWidget::mouseReleaseEvent(event);
        return;
    }

    if (this->messageLayout_->flags.has(MessageLayoutFlag::Collapsed))
    {
        return;
    }

    const auto *hoverElement = this->messageLayout_->getElementAt(event->pos());
    this->handleMouseClickFromRelease(event, hoverElement);

    this->update();
}

void MessageView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || this->messageLayout_ == nullptr ||
        this->messageLayout_->flags.has(MessageLayoutFlag::Collapsed))
    {
        BaseWidget::mouseDoubleClickEvent(event);
        return;
    }

    this->isDoubleClick_ = true;
    this->lastDoubleClickPosition_ = event->globalPosition();
    this->clickTimer_.start();

    const auto *hoverLayoutElement =
        this->messageLayout_->getElementAt(event->pos());

    if (hoverLayoutElement == nullptr)
    {
        const size_t idx =
            this->messageLayout_->getSelectionIndex(QPointF(event->pos()));
        const SelectionItem item(0, idx);
        this->doubleClickSelection_ = {item, item};
        return;
    }

    auto [wordStart, wordEnd] =
        this->messageLayout_->getWordBounds(hoverLayoutElement, event->pos());

    this->doubleClickSelection_ = {
        SelectionItem(0, static_cast<size_t>(wordStart)),
        SelectionItem(0, static_cast<size_t>(wordEnd))};
    this->setSelection(this->doubleClickSelection_);

    if (getSettings()->linksDoubleClickOnly.getValue() &&
        hoverLayoutElement->getLink().isValid())
    {
        this->handleLinkClick(event, hoverLayoutElement->getLink());
    }
}

void MessageView::paintEvent(QPaintEvent * /*event*/)
{
    if (this->messageLayout_ == nullptr)
    {
        this->hasAnimatedElements_ = false;
        return;
    }

    QPainter painter(this);

    auto ctx = MessagePaintContext{
        .painter = painter,
        .selection = this->selection_,
        .colorProvider = ColorProvider::instance(),
        .messageColors = this->messageColors_,
        .preferences = this->messagePreferences_,

        .canvasWidth = this->width_,
        .isWindowFocused = this->window() == QApplication::activeWindow(),
        .isMentions = false,

        .y = 0,
        .messageIndex = 0,
        .isLastReadMessage = false,
    };

    const auto result = this->messageLayout_->paint(ctx);
    this->hasAnimatedElements_ = result.hasAnimatedElements;
}

void MessageView::themeChangedEvent()
{
    BaseWidget::themeChangedEvent();
    this->messageColors_.applyTheme(getTheme(), false, 255);
    this->messageColors_.regularBg = getTheme()->splits.input.background;
    if (this->messageLayout_)
    {
        this->messageLayout_->invalidateBuffer();
    }
}

void MessageView::scaleChangedEvent(float newScale)
{
    (void)newScale;

    this->layoutMessage();
}

void MessageView::layoutMessage()
{
    if (this->messageLayout_ == nullptr)
    {
        this->setFixedHeight(0);
        return;
    }

    const auto flags = this->layoutUsesChatWordFlags_
                           ? getApp()->getWindows()->getWordFlags()
                           : MessageElementFlags(MESSAGE_FLAGS);

    const bool updateRequired = this->messageLayout_->layout(
        {
            .messageColors = this->messageColors_,
            .flags = flags,
            .width = this->width_,
            .scale = this->scale(),
            .imageScale =
                this->scale() * static_cast<float>(this->devicePixelRatio()),
            .selectedChannel = nullptr,
            .message = *this->message_,
        },
        false);

    if (updateRequired)
    {
        this->setFixedSize(this->width_, this->messageLayout_->getHeight());
        this->update();
    }
}

}  // namespace chatterino

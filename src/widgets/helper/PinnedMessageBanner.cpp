#include "widgets/helper/PinnedMessageBanner.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "messages/layouts/MessageLayout.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/Link.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/Scrollbar.hpp"
#include "widgets/splits/Split.hpp"

#include <IrcMessage>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace chatterino {

namespace {
constexpr int BASE_ICON_SIZE = 14;
constexpr int BASE_HEADER_FONT_SIZE = 11;
constexpr int BASE_COLLAPSED_MSG_HEIGHT = 28;
constexpr int MAX_EXPANDED_HEIGHT = 500;
constexpr int BASE_TOP_MARGIN = 4;
constexpr int BASE_BOTTOM_MARGIN = 7;
constexpr int BASE_HEADER_LEFT_MARGIN = 8;
constexpr int BASE_MESSAGE_LEFT_MARGIN = 0;
constexpr int BASE_RIGHT_MARGIN = 8;
constexpr int BASE_SPACING = 3;
constexpr int BASE_HEADER_SPACING = 2;
constexpr int BASE_HEADER_TEXT_BOTTOM_INSET = 0;

constexpr float BASE_TEXT_SCALE = 0.80f;
constexpr float BASE_BADGE_SCALE = 0.25f;
constexpr float BASE_EMOTE_SCALE = 0.55f;

float normalizedBannerScale(float value)
{
    if (!std::isfinite(value))
    {
        return 1.F;
    }
    return std::clamp(value, 0.5F, 2.F);
}

float pinnedMessageScale()
{
    return normalizedBannerScale(float(getSettings()->pinnedMessageScale));
}

float pinnedContentScale()
{
    return normalizedBannerScale(float(getSettings()->pinnedContentScale));
}

int scaledInt(float value, int minimum = 1)
{
    return std::max(minimum, int(std::round(value)));
}

bool sameOptionalFloat(const std::optional<float> &current, float value)
{
    return current.has_value() && std::abs(*current - value) < 0.0001F;
}

void setPinnedBadgeFlag(MessageElementFlags &flags,
                        const MessageElementFlags &wordFlags,
                        MessageElementFlag flag)
{
    if (wordFlags.has(flag))
    {
        flags.set(flag);
    }
}

MessageElementFlags pinnedMessageFlags()
{
    MessageElementFlags flags{
        MessageElementFlag::Text,       MessageElementFlag::AlwaysShow,
        MessageElementFlag::EmoteImage, MessageElementFlag::EmojiAll,
        MessageElementFlag::Collapsed,  MessageElementFlag::Username,
    };

    const auto wordFlags = getApp()->getWindows()->getWordFlags();
    setPinnedBadgeFlag(flags, wordFlags,
                       MessageElementFlag::BadgeSharedChannel);
    setPinnedBadgeFlag(flags, wordFlags,
                       MessageElementFlag::BadgeGlobalAuthority);
    setPinnedBadgeFlag(flags, wordFlags, MessageElementFlag::BadgePredictions);
    setPinnedBadgeFlag(flags, wordFlags,
                       MessageElementFlag::BadgeChannelAuthority);
    setPinnedBadgeFlag(flags, wordFlags, MessageElementFlag::BadgeSubscription);
    setPinnedBadgeFlag(flags, wordFlags, MessageElementFlag::BadgeVanity);
    setPinnedBadgeFlag(flags, wordFlags, MessageElementFlag::BadgeChatterino);
    setPinnedBadgeFlag(flags, wordFlags, MessageElementFlag::BadgeSevenTV);
    setPinnedBadgeFlag(flags, wordFlags, MessageElementFlag::BadgeFfz);
    setPinnedBadgeFlag(flags, wordFlags, MessageElementFlag::BadgeBttv);
    setPinnedBadgeFlag(flags, wordFlags,
                       MessageElementFlag::BadgeHomiesSupporter);
    setPinnedBadgeFlag(flags, wordFlags, MessageElementFlag::BadgeHomiesCustom);
    setPinnedBadgeFlag(flags, wordFlags, MessageElementFlag::BadgeMoltorino);
    setPinnedBadgeFlag(flags, wordFlags, MessageElementFlag::BadgeFolhinha);
    setPinnedBadgeFlag(flags, wordFlags, MessageElementFlag::BadgeFfzAp);
    setPinnedBadgeFlag(flags, wordFlags, MessageElementFlag::BadgeDankChat);
    setPinnedBadgeFlag(flags, wordFlags, MessageElementFlag::BadgeChatsen);

    return flags;
}
}  // namespace

namespace {

const MessageLayoutElement *tryGetPinnedBannerElementAt(ChannelView *view,
                                                        QPointF pos)
{
    if (view == nullptr)
    {
        return nullptr;
    }

    auto &messages = view->getMessagesSnapshot();
    const auto start = size_t(view->getScrollBar().getRelativeCurrentValue());
    if (start >= messages.size())
    {
        return nullptr;
    }

    qreal y = -(messages[start]->getHeight() *
                fmod(view->getScrollBar().getRelativeCurrentValue(), 1));

    for (size_t i = start; i < messages.size(); ++i)
    {
        const auto &layout = messages[i];
        if (pos.y() < y + layout->getHeight())
        {
            return layout->getElementAt(QPointF(pos.x(), pos.y() - y));
        }

        y += layout->getHeight();
    }

    return nullptr;
}

bool shouldLetEmbeddedMessageHandleClick(ChannelView *view, QMouseEvent *event)
{
    if (event == nullptr)
    {
        return false;
    }

    const auto *element = tryGetPinnedBannerElementAt(view, event->position());
    if (element == nullptr)
    {
        return false;
    }

    const auto &link = element->getLink();
    return link.type != Link::None;
}

}  // namespace

PinnedMessageBanner::PinnedMessageBanner(Split *split, QWidget *parent)
    : BaseWidget(parent)
    , split_(split)
{
    this->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    this->managedConnections_.emplace_back(split->focused.connect([this]() {
        this->update();
    }));
    this->managedConnections_.emplace_back(split->focusLost.connect([this]() {
        this->update();
    }));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, BASE_TOP_MARGIN, BASE_RIGHT_MARGIN,
                               BASE_BOTTOM_MARGIN);
    layout->setSpacing(BASE_SPACING);

    this->topLayout_ = new QHBoxLayout();
    this->topLayout_->setContentsMargins(BASE_HEADER_LEFT_MARGIN, 0, 0, 0);
    this->topLayout_->setSpacing(BASE_HEADER_SPACING);

    this->icon_ = new SvgButton(
        {
            .dark = ":/buttons/pinEnabled.svg",
            .light = ":/buttons/pinEnabled.svg",
        },
        this, QSize(2, 2));
    this->icon_->setFixedSize(BASE_ICON_SIZE, BASE_ICON_SIZE);
    this->icon_->setCursor(Qt::ArrowCursor);
    this->icon_->setGraphicsEffect(new QGraphicsOpacityEffect);
    static_cast<QGraphicsOpacityEffect *>(this->icon_->graphicsEffect())
        ->setOpacity(0.5);
    this->icon_->installEventFilter(this);
    this->topLayout_->addWidget(this->icon_, 0, Qt::AlignBottom);

    this->pinnerLabel_ = new QLabel(this);
    this->pinnerLabel_->setContentsMargins(0, 0, 0, 0);
    this->pinnerLabel_->setAlignment(Qt::AlignLeft | Qt::AlignBottom);

    this->pinnerLabel_->setStyleSheet(
        QString("font-size: %1px;").arg(BASE_HEADER_FONT_SIZE));

    this->pinnerLabel_->setSizePolicy(QSizePolicy::Expanding,
                                      QSizePolicy::Preferred);
    this->topLayout_->addWidget(this->pinnerLabel_, 1, Qt::AlignBottom);

    this->moreLabel_ = new QLabel(this);
    this->moreLabel_->setContentsMargins(0, 0, 4, 0);
    this->moreLabel_->setAlignment(Qt::AlignRight | Qt::AlignBottom);
    this->moreLabel_->setStyleSheet(
        QString("font-size: %1px; font-weight: bold;")
            .arg(BASE_HEADER_FONT_SIZE));
    this->moreLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    this->moreLabel_->hide();
    this->topLayout_->addWidget(this->moreLabel_, 0, Qt::AlignBottom);

    this->timerLabel_ = new QLabel(this);
    this->timerLabel_->setContentsMargins(0, 0, 4, 0);
    this->timerLabel_->setAlignment(Qt::AlignRight | Qt::AlignBottom);
    this->timerLabel_->setStyleSheet(
        QString("font-size: %1px;").arg(BASE_HEADER_FONT_SIZE));
    this->timerLabel_->hide();
    this->topLayout_->addWidget(this->timerLabel_, 0, Qt::AlignBottom);

    this->toggleButton_ = new SvgButton(
        {
            .dark = ":/buttons/switchLight.svg",
            .light = ":/buttons/switchDark.svg",
        },
        this, QSize(2, 2));
    this->toggleButton_->setToolTip("Switch banner");
    this->toggleButton_->setFixedSize(BASE_ICON_SIZE, BASE_ICON_SIZE);
    auto *toggleOpacity = new QGraphicsOpacityEffect(this);
    toggleOpacity->setOpacity(0.5);
    this->toggleButton_->setGraphicsEffect(toggleOpacity);
    this->toggleButton_->hide();
    this->topLayout_->addWidget(this->toggleButton_, 0, Qt::AlignBottom);

    this->unpinButton_ = new SvgButton(
        {
            .dark = ":/buttons/cancel.svg",
            .light = ":/buttons/cancelDark.svg",
        },
        this, QSize(2, 2));
    this->unpinButton_->setToolTip("Dismiss");
    this->unpinButton_->setFixedSize(BASE_ICON_SIZE, BASE_ICON_SIZE);
    this->unpinButton_->setGraphicsEffect(new QGraphicsOpacityEffect);
    static_cast<QGraphicsOpacityEffect *>(this->unpinButton_->graphicsEffect())
        ->setOpacity(0.5);
    this->unpinButton_->setVisible(true);
    this->topLayout_->addWidget(this->unpinButton_, 0, Qt::AlignBottom);

    layout->addLayout(this->topLayout_);

    this->channel_ = std::make_shared<Channel>("", Channel::Type::None);
    this->messageView_ = new ChannelView(this, split);
    this->messageView_->setOverrideScale(BASE_TEXT_SCALE);
    this->messageView_->setOverrideEmoteScale(BASE_EMOTE_SCALE);
    this->messageView_->setCenterBadges(true);
    this->messageView_->setOverrideSeparateMessages(false);
    this->messageView_->setTransparentBackground(true);
    this->messageView_->installEventFilter(this);
    this->messageView_->setChannel(this->channel_);
    this->messageView_->setEnableScrollingToBottom(false);
    this->messageView_->getScrollBar().hide();  // Ensure no scrollbar

    this->signalHolder_.managedConnect(this->theme->updated, [this]() {
        this->messageView_->setTransparentBackground(true);
    });

    this->managedConnections_.emplace_back(
        this->messageView_->mouseDown.connect([this](QMouseEvent *event) {
            if (event->button() == Qt::LeftButton &&
                !shouldLetEmbeddedMessageHandleClick(this->messageView_, event))
            {
                this->toggleExpansion();
            }
        }));
    this->messageView_->setOverrideFlags(pinnedMessageFlags());
    this->signalHolder_.managedConnect(
        getApp()->getWindows()->wordFlagsChanged, [this]() {
            this->messageView_->setOverrideFlags(pinnedMessageFlags());
            this->messageView_->queueLayout();
        });
    this->messageView_->setCollapseMessages(true);

    this->messageView_->getScrollBar().setFixedHeight(0);
    this->messageView_->getScrollBar().setFixedWidth(0);

    this->messageView_->setFixedHeight(BASE_COLLAPSED_MSG_HEIGHT);
    this->messageView_->setSizePolicy(QSizePolicy::Expanding,
                                      QSizePolicy::Fixed);

    this->messageLayout_ = new QHBoxLayout();
    this->messageLayout_->setContentsMargins(BASE_MESSAGE_LEFT_MARGIN, 0, 0, 0);
    this->messageLayout_->addWidget(this->messageView_);
    layout->addLayout(this->messageLayout_);

    this->hide();

    getSettings()->pinnedMessageScale.connect(
        [this](const float &, auto) {
            if (this->isVisible())
            {
                this->updateScaling();
                this->updateGeometry();
            }
        },
        this->managedConnections_);

    getSettings()->pinnedContentScale.connect(
        [this](const float &, auto) {
            if (this->isVisible())
            {
                this->scaleChangedEvent(this->scale());
                this->updateGeometry();
            }
        },
        this->managedConnections_);

    getSettings()->alwaysExpandPinnedMessages.connect(
        [this](const bool &enabled, auto) {
            if (!this->isVisible())
            {
                return;
            }
            if (enabled)
            {
                this->userManuallyCollapsed_ = false;
                this->isExpanded_ = true;
                this->messageView_->setCollapseMessages(false);
            }
            else
            {
                this->isExpanded_ = false;
                this->messageView_->setCollapseMessages(true);
            }
            this->updateScaling();
            this->updateGeometry();
        },
        this->managedConnections_);

    this->countdownTimer_ = new QTimer(this);
    this->countdownTimer_->setInterval(1000);
    this->connect(this->countdownTimer_, &QTimer::timeout, [this]() {
        this->updateTimer();
    });

    getSettings()->pinTimerDisplay.connect(
        [this](const int &, auto) {
            if (this->isVisible())
            {
                this->updateTimer();
            }
        },
        this->managedConnections_);

    getSettings()->pinTimestampFormat.connect(
        [this](const QString &, auto) {
            if (this->isVisible())
            {
                this->updateTimer();
            }
        },
        this->managedConnections_);

    getSettings()->pinBannerBackgroundColor.connect(
        [this](const QString &, auto) {
            this->update();  // Repaint with new background
        },
        this->managedConnections_);

    QObject::connect(this->toggleButton_, &Button::leftClicked, [this]() {
        this->toggleBannerRequested.invoke();
    });
}

bool PinnedMessageBanner::hasPinnedMessage() const
{
    return this->hasPin_;
}

void PinnedMessageBanner::clearDismissState()
{
    this->dismissedPinId_.clear();
}

void PinnedMessageBanner::setToggleButtonVisible(bool visible)
{
    this->toggleButton_->setVisible(visible);
}

void PinnedMessageBanner::setPinnedMessage(
    const std::optional<TwitchChannel::PinnedMessage> &pin,
    TwitchChannel *channel)
{
    if (!pin || !channel)
    {
        this->hasPin_ = false;
        this->dismissedPinId_.clear();
        this->currentPinMessageId_.clear();
        this->endsAt_ = std::nullopt;
        this->pinnedAt_.reset();
        this->countdownTimer_->stop();
        this->twitchChannel_ = nullptr;
        this->initialLayoutStabilizationQueued_ = false;
        this->setUpdatesEnabled(true);
        this->hide();
        return;
    }

    this->twitchChannel_ = channel;
    if (this->split_ != nullptr)
    {
        this->messageView_->setSourceChannel(this->split_->getChannel());
    }
    this->endsAt_ = pin->endsAt;

    if (this->currentPinMessageId_ != pin->messageId ||
        !this->pinnedAt_.has_value())
    {
        this->pinnedAt_ = pin->pinnedAt;
    }
    this->currentPinMessageId_ = pin->messageId;

    if (!this->dismissedPinId_.isEmpty() &&
        this->dismissedPinId_ == pin->messageId)
    {
        this->hasPin_ = false;
        this->initialLayoutStabilizationQueued_ = false;
        this->setUpdatesEnabled(true);
        this->hide();
        return;
    }
    this->dismissedPinId_.clear();
    this->hasPin_ = true;

    this->countdownTimer_->stop();

    this->channel_->clearMessages();

    MessagePtrMut message;
    MessagePtr originalMessage;
    if (!pin->messageId.isEmpty())
    {
        originalMessage = channel->findMessageByID(pin->messageId);
    }

    if (originalMessage)
    {
        message = originalMessage->clone();
    }
    else
    {
        QString author =
            pin->authorLogin.isEmpty() ? "Unknown" : pin->authorLogin;

        QStringList tags;
        if (!pin->messageId.isEmpty())
            tags << "id=" + pin->messageId;
        if (!pin->authorId.isEmpty())
            tags << "user-id=" + pin->authorId;
        if (!pin->authorColor.isEmpty())
            tags << "color=" + pin->authorColor;
        if (!pin->authorBadges.isEmpty())
            tags << "badges=" + pin->authorBadges;
        if (!channel->roomId().isEmpty())
            tags << "room-id=" + channel->roomId();
        if (!pin->authorLogin.isEmpty())
            tags << "login=" + pin->authorLogin;
        if (!pin->authorName.isEmpty())
            tags << "display-name=" + pin->authorName;

        QString tagsStr = tags.isEmpty() ? "" : "@" + tags.join(";") + " ";
        QString fakeIrcData =
            QString("%1:%2!%2@%2.tmi.twitch.tv PRIVMSG #%3 :%4")
                .arg(tagsStr, author, channel->getName(), pin->text);

        auto *fakeMessage =
            Communi::IrcMessage::fromData(fakeIrcData.toUtf8(), nullptr);

        if (fakeMessage && fakeMessage->command() == "PRIVMSG")
        {
            MessageParseArgs args;
            std::tie(message, std::ignore) = MessageBuilder::makeIrcMessage(
                channel, fakeMessage, args, pin->text, 0);
        }

        if (fakeMessage)
        {
            fakeMessage->deleteLater();
        }
    }

    if (message)
    {
        message->flags.unset(MessageFlag::RecentMessage);
        message->flags.unset(MessageFlag::Disabled);
        message->flags.unset(MessageFlag::Highlighted);
        message->flags.unset(MessageFlag::RedeemedHighlight);
        message->flags.unset(MessageFlag::FirstMessage);
        message->flags.unset(MessageFlag::ElevatedMessage);

        auto it =
            std::remove_if(message->elements.begin(), message->elements.end(),
                           [](const std::unique_ptr<MessageElement> &el) {
                               auto flags = el->getFlags();
                               return flags.hasAny(MessageElementFlags{
                                   MessageElementFlag::Timestamp,
                                   MessageElementFlag::ChannelName});
                           });
        message->elements.erase(it, message->elements.end());

        this->channel_->addMessage(message, MessageContext::Original);
    }

    this->pinnerName_ =
        pin->pinnerName.isEmpty() ? "Moderator" : pin->pinnerName;
    this->pinnerLabel_->setText("Pinned by " + this->pinnerName_);

    int displayMode = getSettings()->pinTimerDisplay;

    if (displayMode <= 2 && this->endsAt_.has_value())
    {
        this->icon_->setToolTip(QString());
    }
    else if (!this->endsAt_.has_value())
    {
        this->icon_->setToolTip(QString("Pinned by %1\nPinned indefinitely")
                                    .arg(this->pinnerName_));
    }
    else
    {
        this->icon_->setToolTip(QString());
    }

    if (displayMode <= 2 &&
        (this->endsAt_.has_value() || this->pinnedAt_.has_value()))
    {
        this->updateTimer();
        this->countdownTimer_->start();
    }
    else
    {
        this->timerLabel_->hide();
    }

    this->updateScaling();

    bool hasModRights = channel->hasModRights();

    this->unpinButton_->disconnect();
    this->unpinButton_->setVisible(true);
    this->connect(this->unpinButton_, &Button::leftClicked,
                  [this, channel, pin, hasModRights]() {
                      int action = getSettings()->pinCloseButtonAction;
                      if (action == 1 && hasModRights)
                      {
                          channel->unpinMessage();
                      }
                      else
                      {
                          this->dismissedPinId_ = pin->messageId;
                          this->hasPin_ = false;
                          this->countdownTimer_->stop();
                          this->hide();
                          this->dismissed.invoke();
                      }
                  });
    this->unpinButton_->setToolTip(
        (getSettings()->pinCloseButtonAction == 1 && hasModRights)
            ? "Unpin message"
            : "Dismiss");

    this->icon_->disconnect();
    if (hasModRights)
    {
        this->icon_->setCursor(Qt::PointingHandCursor);
        this->connect(this->icon_, &Button::leftClicked,
                      [this, channel, pin]() {
                          auto *menu = new QMenu(this);
                          menu->setAttribute(Qt::WA_DeleteOnClose);

                          menu->addAction("Unpin Message", [channel]() {
                              channel->unpinMessage();
                          });

                          if (pin->endsAt.has_value())
                          {
                              menu->addSeparator();
                              menu->addAction("Pin Indefinitely", [channel]() {
                                  channel->keepPinned();
                              });
                          }

                          menu->popup(QCursor::pos());
                      });
    }
    else
    {
        this->icon_->setCursor(Qt::ArrowCursor);
    }

    this->userManuallyCollapsed_ = false;

    if (getSettings()->alwaysExpandPinnedMessages &&
        !this->userManuallyCollapsed_)
    {
        this->isExpanded_ = true;
        this->messageView_->setCollapseMessages(false);
    }

    this->scheduleInitialLayoutStabilization();
}

void PinnedMessageBanner::showEvent(QShowEvent *event)
{
    BaseWidget::showEvent(event);
    this->scaleChangedEvent(this->scale());
    this->scheduleInitialLayoutStabilization();
}

void PinnedMessageBanner::scaleChangedEvent(float scale)
{
    BaseWidget::scaleChangedEvent(scale);

    const float contentScale = scale * pinnedContentScale();
    int iconSize = scaledInt(BASE_ICON_SIZE * contentScale);
    this->headerFontSize_ = scaledInt(BASE_HEADER_FONT_SIZE * contentScale);

    this->icon_->setFixedSize(iconSize, iconSize);
    this->unpinButton_->setFixedSize(iconSize, iconSize);
    this->toggleButton_->setFixedSize(iconSize, iconSize);

    if (auto *lay = qobject_cast<QVBoxLayout *>(this->layout()))
    {
        lay->setContentsMargins(
            0, scaledInt(BASE_TOP_MARGIN * contentScale, 0),
            scaledInt(BASE_RIGHT_MARGIN * contentScale, 0),
            scaledInt(BASE_BOTTOM_MARGIN * contentScale, 0));
        lay->setSpacing(scaledInt(BASE_SPACING * contentScale, 0));
    }
    if (this->topLayout_ != nullptr)
    {
        this->topLayout_->setContentsMargins(
            scaledInt(BASE_HEADER_LEFT_MARGIN * contentScale, 0), 0, 0, 0);
        this->topLayout_->setSpacing(
            scaledInt(BASE_HEADER_SPACING * contentScale, 0));
    }
    const int textBottomInset =
        scaledInt(BASE_HEADER_TEXT_BOTTOM_INSET * contentScale, 0);
    this->pinnerLabel_->setContentsMargins(0, 0, 0, textBottomInset);
    this->moreLabel_->setContentsMargins(0, 0, scaledInt(4 * contentScale, 0),
                                         textBottomInset);
    this->timerLabel_->setContentsMargins(0, 0, scaledInt(4 * contentScale, 0),
                                          textBottomInset);

    this->updateLabelStyles();
    this->updateScaling();
}

void PinnedMessageBanner::themeChangedEvent()
{
    this->updateLabelStyles();

    QColor textColor = this->theme->splits.header.text;
    this->icon_->setColor(textColor);
    if (auto *eff = qobject_cast<QGraphicsOpacityEffect *>(
            this->icon_->graphicsEffect()))
    {
        eff->setOpacity(0.55);
    }

    this->update();
}

void PinnedMessageBanner::updateLabelStyles()
{
    QColor textColor = this->theme->splits.header.text;
    auto colorStr = QString("rgba(%1, %2, %3, 0.55)")
                        .arg(textColor.red())
                        .arg(textColor.green())
                        .arg(textColor.blue());
    int fs = this->headerFontSize_;

    this->pinnerLabel_->setStyleSheet(
        QString("font-size: %1px; color: %2;").arg(fs).arg(colorStr));
    this->timerLabel_->setStyleSheet(
        QString("font-size: %1px; color: %2;").arg(fs).arg(colorStr));
    this->moreLabel_->setStyleSheet(
        QString("font-size: %1px; font-weight: bold; color: %2;")
            .arg(fs)
            .arg(colorStr));
}

void PinnedMessageBanner::scheduleInitialLayoutStabilization()
{
    if (!this->hasPin_ || this->initialLayoutStabilizationQueued_)
    {
        return;
    }

    this->initialLayoutStabilizationQueued_ = true;
    this->setUpdatesEnabled(false);

    QTimer::singleShot(0, this, [this] {
        this->initialLayoutStabilizationQueued_ = false;

        if (this->hasPin_)
        {
            this->updateScaling();
            this->updateGeometry();
        }

        this->setUpdatesEnabled(true);
        this->update();
    });
}

void PinnedMessageBanner::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);

    QString customBg = getSettings()->pinBannerBackgroundColor;
    QColor background;
    if (!customBg.isEmpty() && QColor::isValidColorName(customBg))
    {
        background = QColor(customBg);
    }
    else
    {
        background = this->theme->splits.header.background;
    }

    QColor border = this->theme->splits.header.border;
    if (this->split_->hasFocus())
    {
        border = this->theme->splits.header.focusedBorder;
    }

    painter.fillRect(this->rect(), background);
    painter.setPen(border);
    painter.drawRect(0, 0, this->width() - 1, this->height() - 1);
}

void PinnedMessageBanner::resizeEvent(QResizeEvent *event)
{
    BaseWidget::resizeEvent(event);
    this->updateScaling();

    if (this->isExpanded_)
    {
        auto &snapshot = this->messageView_->getMessagesSnapshot();
        if (!snapshot.empty())
        {
            int contentHeight = snapshot[0]->getHeight();
            int firstLineHeight = snapshot[0]->getFirstLineHeight();
            bool isTruncated = contentHeight > firstLineHeight;

            if (!isTruncated)
            {
                this->isExpanded_ = false;
                this->messageView_->setCollapseMessages(true);
                this->messageView_->setFixedHeight(firstLineHeight);
            }
            else
            {
                this->messageView_->setFixedHeight(
                    std::min(contentHeight, MAX_EXPANDED_HEIGHT));
            }
        }
    }
}

void PinnedMessageBanner::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        this->toggleExpansion();
    }
    BaseWidget::mousePressEvent(event);
}

void PinnedMessageBanner::toggleExpansion()
{
    this->split_->setFocus(Qt::MouseFocusReason);

    auto &snapshot = this->messageView_->getMessagesSnapshot();
    if (snapshot.empty())
    {
        return;
    }

    int contentHeight = snapshot[0]->getHeight() + 4;
    int firstLineHeight = snapshot[0]->getFirstLineHeight();
    bool isTruncated = contentHeight > firstLineHeight + 4;

    if (!isTruncated)
    {
        this->isExpanded_ = false;
        this->messageView_->setCollapseMessages(true);
        this->messageView_->setFixedHeight(firstLineHeight);
        this->updateGeometry();
        return;
    }

    this->isExpanded_ = !this->isExpanded_;
    this->messageView_->setCollapseMessages(!this->isExpanded_);

    if (!this->isExpanded_ && getSettings()->alwaysExpandPinnedMessages)
    {
        this->userManuallyCollapsed_ = true;
    }
    else
    {
        this->userManuallyCollapsed_ = false;
    }

    this->updateScaling();
    this->updateGeometry();
}

void PinnedMessageBanner::refreshLayout()
{
    if (!this->isVisible())
    {
        return;
    }

    auto &snapshot = this->messageView_->getMessagesSnapshot();
    for (size_t i = 0; i < snapshot.size(); i++)
    {
        snapshot[i]->flags.set(MessageLayoutFlag::RequiresLayout);
    }
    this->messageView_->performLayout();
    this->messageView_->update();
}

void PinnedMessageBanner::updateScaling()
{
    if (this->width() <= 0)
    {
        return;
    }

    auto &snapshot = this->messageView_->getMessagesSnapshot();
    if (snapshot.empty())
    {
        return;
    }

    float s = this->scale();
    float userScale = pinnedMessageScale();
    float combined = s * userScale;
    float overrideScale = BASE_TEXT_SCALE * combined;
    if (!sameOptionalFloat(this->messageView_->overrideScale(), overrideScale))
    {
        this->messageView_->setOverrideScale(overrideScale);
    }

    const float badgeScale = BASE_BADGE_SCALE * combined;
    if (!sameOptionalFloat(this->messageView_->getOverrideBadgeScale(),
                           badgeScale))
    {
        this->messageView_->setOverrideBadgeScale(badgeScale);
    }

    const float emoteScale = BASE_EMOTE_SCALE * combined;
    if (!sameOptionalFloat(this->messageView_->getOverrideEmoteScale(),
                           emoteScale))
    {
        this->messageView_->setOverrideEmoteScale(emoteScale);
    }

    if (this->messageLayout_ != nullptr)
    {
        const float contentScale = s * pinnedContentScale();
        int internalLeft = scaledInt(8.0f * overrideScale, 0);
        int targetLeft = scaledInt(BASE_HEADER_LEFT_MARGIN * contentScale, 0);
        this->messageLayout_->setContentsMargins(
            std::max(0, targetLeft - internalLeft), 0, 0, 0);
    }

    this->messageView_->performLayout();

    if (!snapshot.empty())
    {
        int contentHeight = snapshot[0]->getHeight();
        int firstLineHeight = snapshot[0]->getFirstLineHeight();
        bool isTruncated = contentHeight > firstLineHeight;

        if (isTruncated && getSettings()->alwaysExpandPinnedMessages &&
            !this->isExpanded_ && !this->userManuallyCollapsed_)
        {
            this->isExpanded_ = true;
            this->messageView_->setCollapseMessages(false);
            this->messageView_->performLayout();
            contentHeight = snapshot[0]->getHeight();
        }

        if (!isTruncated && this->isExpanded_)
        {
            this->isExpanded_ = false;
        }

        if (this->isExpanded_)
        {
            this->messageView_->setFixedHeight(
                std::min(contentHeight, MAX_EXPANDED_HEIGHT));
            this->moreLabel_->setText(QString::fromUtf8("\xE2\x96\xB2 Less"));
        }
        else
        {
            this->messageView_->setFixedHeight(firstLineHeight);
            this->moreLabel_->setText(QString::fromUtf8("\xE2\x96\xBC More"));
        }

        this->moreLabel_->setVisible(isTruncated);
    }
}

bool PinnedMessageBanner::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == this->messageView_ && event->type() == QEvent::Wheel)
    {
        auto *wheelEvent = static_cast<QWheelEvent *>(event);
        if (wheelEvent->modifiers() & Qt::ControlModifier)
        {
            return false;
        }
        return true;
    }

    if (obj == this->icon_)
    {
        int mode = getSettings()->pinTimerDisplay;
        if (event->type() == QEvent::Enter)
        {
            if (mode == 3 &&
                (this->endsAt_.has_value() || this->pinnedAt_.has_value()))
            {
                this->updateTimer();
                this->countdownTimer_->start();
            }
            else if ((mode == 1 || mode == 2) &&
                     (this->endsAt_.has_value() || this->pinnedAt_.has_value()))
            {
                this->updateTimer();
            }
        }
        else if (event->type() == QEvent::Leave)
        {
            if (mode == 3)
            {
                this->countdownTimer_->stop();
            }
            QToolTip::hideText();
        }
    }

    return BaseWidget::eventFilter(obj, event);
}

namespace {

QString formatElapsed(qint64 totalSecs)
{
    if (totalSecs < 60)
        return "just now";
    if (totalSecs < 3600)
        return QString("%1m ago").arg(totalSecs / 60);
    if (totalSecs < 86400)
    {
        int h = totalSecs / 3600;
        int m = (totalSecs % 3600) / 60;
        return m > 0 ? QString("%1h %2m ago").arg(h).arg(m)
                     : QString("%1h ago").arg(h);
    }
    return QString("%1d ago").arg(totalSecs / 86400);
}

QString formatRemaining(qint64 secs)
{
    int mins = static_cast<int>(secs / 60);
    int s = static_cast<int>(secs % 60);
    return QString("%1:%2").arg(mins).arg(s, 2, 10, QChar('0'));
}

}  // namespace

void PinnedMessageBanner::updateTimer()
{
    int displayMode = getSettings()->pinTimerDisplay;

    if (displayMode == 4)
    {
        this->timerLabel_->hide();
        this->countdownTimer_->stop();
        return;
    }

    auto now = QDateTime::currentDateTimeUtc();

    QString timeStr;
    if (this->pinnedAt_.has_value())
    {
        QString format = getSettings()->pinTimestampFormat;
        if (format == "Relative")
        {
            qint64 elapsed = this->pinnedAt_->secsTo(now);
            if (elapsed >= 0)
                timeStr = formatElapsed(elapsed);
        }
        else
        {
            QDateTime localTime = this->pinnedAt_->toLocalTime();
            timeStr = localTime.toString(format);
        }
    }

    QString countdownStr;
    if (this->endsAt_.has_value())
    {
        qint64 remaining = now.secsTo(*this->endsAt_);
        if (remaining <= 0)
        {
            countdownStr = "Expired";
            this->countdownTimer_->stop();
            if (this->twitchChannel_)
                this->twitchChannel_->refreshPinnedMessage();
        }
        else
        {
            countdownStr = formatRemaining(remaining);
        }
    }

    if ((displayMode == 0 || displayMode == 1) && !timeStr.isEmpty())
    {
        this->pinnerLabel_->setText(QString("Pinned by %1 \xC2\xB7 %2")
                                        .arg(this->pinnerName_, timeStr));
    }
    else
    {
        this->pinnerLabel_->setText("Pinned by " + this->pinnerName_);
    }

    if ((displayMode == 0 || displayMode == 2) && !countdownStr.isEmpty())
    {
        this->timerLabel_->setText(countdownStr);
        this->timerLabel_->show();
    }
    else if (displayMode == 2 && !this->endsAt_.has_value())
    {
        this->timerLabel_->setText("Indefinitely");
        this->timerLabel_->show();
    }
    else
    {
        this->timerLabel_->hide();
    }

    if (displayMode == 3 && this->icon_->underMouse())
    {
        QString tooltip;
        if (!timeStr.isEmpty() && !countdownStr.isEmpty())
            tooltip =
                QString("%1 \xC2\xB7 %2 remaining").arg(timeStr, countdownStr);
        else if (!countdownStr.isEmpty())
            tooltip = QString("%1 remaining").arg(countdownStr);
        else if (!timeStr.isEmpty())
            tooltip = timeStr;
        else
            tooltip = "Pinned indefinitely";

        QToolTip::showText(
            this->icon_->mapToGlobal(QPoint(0, this->icon_->height() + 4)),
            tooltip, this->icon_);
    }
    else if (displayMode == 1 && this->icon_->underMouse() &&
             this->endsAt_.has_value())
    {
        QToolTip::showText(
            this->icon_->mapToGlobal(QPoint(0, this->icon_->height() + 4)),
            QString("%1 remaining").arg(countdownStr), this->icon_);
    }
    else if (displayMode == 2 && this->icon_->underMouse() &&
             !timeStr.isEmpty())
    {
        QToolTip::showText(
            this->icon_->mapToGlobal(QPoint(0, this->icon_->height() + 4)),
            timeStr, this->icon_);
    }
}

}  // namespace chatterino

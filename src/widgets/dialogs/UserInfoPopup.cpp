// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/UserInfoPopup.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/Literals.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/builtin/Misc.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/highlights/HighlightBlacklistUser.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/userdata/UserDataController.hpp"
#include "messages/Link.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/IvrApi.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickApi.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/pronouns/Pronouns.hpp"
#include "providers/seventv/paints/Paint.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/twitch/TwitchNameHistory.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Clipboard.hpp"
#include "util/FormatTime.hpp"
#include "util/Helpers.hpp"
#include "util/LayoutCreator.hpp"
#include "util/PostToThread.hpp"
#include "widgets/buttons/FollowButton.hpp"
#include "widgets/buttons/LabelButton.hpp"
#include "widgets/buttons/PixmapButton.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/dialogs/EditUserNotesDialog.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/InvisibleSizeGrip.hpp"
#include "widgets/helper/Line.hpp"
#include "widgets/helper/LiveIndicator.hpp"
#include "widgets/helper/ScalingSpacerItem.hpp"
#include "widgets/Label.hpp"
#include "widgets/MarkdownLabel.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/Scrollbar.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/Window.hpp"

#include <IrcMessage>
#include <QCheckBox>
#include <QColor>
#include <QDate>
#include <QDesktopServices>
#include <QFile>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHash>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QMessageBox>
#include <QMetaEnum>
#include <QMouseEvent>
#include <QMovie>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QShowEvent>
#include <QStringBuilder>
#include <QSvgRenderer>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

namespace {
constexpr QStringView TEXT_FOLLOWERS = u"Followers: %1";
constexpr QStringView TEXT_CREATED = u"Created: %1";
constexpr QStringView TEXT_TITLE = u"%1's Usercard - #%2";
constexpr QStringView TEXT_USER_ID = u"ID: ";
constexpr QStringView TEXT_UNAVAILABLE = u"(not available)";
constexpr QStringView TEXT_PRONOUNS = u"Pronouns: %1";
constexpr QStringView TEXT_UNSPECIFIED = u"(unspecified)";
constexpr QStringView TEXT_LOADING = u"(loading...)";
constexpr QStringView TEXT_LAST_LIVE = u"Last Live: %1";
constexpr QStringView TEXT_COLOR = u"Color: %1";
constexpr QStringView TEXT_SEVENTV_PAINT = u"7TV Paint: ";
constexpr QStringView TEXT_CHATTERS = u"Chatters: %1";

constexpr QStringView SEVENTV_TWITCH_USER_API =
    u"https://7tv.io/v3/users/twitch/%1";
constexpr QStringView SEVENTV_KICK_USER_API =
    u"https://7tv.io/v3/users/kick/%1";
constexpr QStringView SEVENTV_USER_PAGE = u"https://7tv.app/users/";
constexpr QStringView SUSGEE_PAINT_PAGE =
    u"https://susgee.dev/paint/%1?utm_source=leafyrino";

using namespace chatterino;

QString chatVaultTwitchChannelUrl(const QString &login)
{
    return QStringLiteral("https://chatvau.lt/channel/twitch/%1")
        .arg(QString::fromLatin1(QUrl::toPercentEncoding(login.toLower())));
}

QString sevenTVUserCacheKey(const QString &userID, bool isKick)
{
    return (isKick ? QStringLiteral("kick:") : QStringLiteral("twitch:")) +
           userID;
}

QHash<QString, QString> &sevenTVUserIDCache()
{
    static QHash<QString, QString> cache;
    return cache;
}

class NameHistoryMenuRow final : public QWidget
{
public:
    NameHistoryMenuRow(QString login, QString leftText, QString rightText,
                       QWidget *parent)
        : QWidget(parent)
        , login_(std::move(login))
    {
        this->setCursor(Qt::PointingHandCursor);
        this->setMouseTracking(true);
        this->setToolTip("Click to copy " + this->login_);

        const auto metrics = this->fontMetrics();
        const auto loginWidth = std::max(
            metrics.horizontalAdvance("koplayzenthraquiluxmorive") + 8, 132);
        const auto dateWidth = metrics.horizontalAdvance("Sep 30, 2026") + 8;
        const auto dashWidth = metrics.horizontalAdvance("-") + 8;

        auto *layout = new QGridLayout(this);
        layout->setContentsMargins(8, 2, 8, 2);
        layout->setHorizontalSpacing(4);
        layout->setVerticalSpacing(0);

        auto *loginLabel = new QLabel(
            metrics.elidedText(this->login_, Qt::ElideRight, loginWidth), this);
        loginLabel->setFixedWidth(loginWidth);
        loginLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        loginLabel->setToolTip(this->login_);
        layout->addWidget(loginLabel, 0, 0, Qt::AlignVCenter);

        auto *leftLabel = new QLabel(std::move(leftText), this);
        leftLabel->setFixedWidth(dateWidth);
        leftLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        leftLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(leftLabel, 0, 1, Qt::AlignVCenter);

        auto *dashLabel = new QLabel("-", this);
        dashLabel->setFixedWidth(dashWidth);
        dashLabel->setAlignment(Qt::AlignCenter);
        dashLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(dashLabel, 0, 2, Qt::AlignVCenter);

        auto *rightLabel = new QLabel(std::move(rightText), this);
        rightLabel->setFixedWidth(dateWidth);
        rightLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        rightLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(rightLabel, 0, 3, Qt::AlignVCenter);

        const auto height = std::max(metrics.height() + 6, 22);
        this->setFixedSize(loginWidth + dateWidth * 2 + dashWidth + 36, height);
    }

protected:
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::Enter)
        {
            this->hovered_ = true;
            this->update();
        }
        else if (event->type() == QEvent::Leave)
        {
            this->hovered_ = false;
            this->update();
        }

        return QWidget::event(event);
    }

    void paintEvent(QPaintEvent *event) override
    {
        if (this->hovered_)
        {
            QPainter painter(this);
            auto highlight = this->palette().color(QPalette::Highlight);
            highlight.setAlpha(70);
            painter.fillRect(this->rect(), highlight);
        }

        QWidget::paintEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton)
        {
            QWidget::mouseReleaseEvent(event);
            return;
        }

        crossPlatformCopy(this->login_);
        QToolTip::showText(event->globalPosition().toPoint(),
                           QString("Copied %1").arg(this->login_), this);

        for (auto *widget = this->parentWidget(); widget != nullptr;
             widget = widget->parentWidget())
        {
            if (auto *menu = qobject_cast<QMenu *>(widget))
            {
                menu->close();
                break;
            }
        }
    }

private:
    QString login_;
    bool hovered_ = false;
};

class ClickableColorRow final : public Button
{
public:
    ClickableColorRow()
        : Button(nullptr)
        , layout_(this)
    {
        this->layout_.setContentsMargins(8, 0, 8, 0);
        this->layout_.setSpacing(5);
        this->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    }

    QHBoxLayout *layout()
    {
        return &this->layout_;
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        // Keep Button's reliable click handling without its hover/click wash.
    }

    void paintContent(QPainter & /*painter*/) override
    {
    }

private:
    QHBoxLayout layout_;
};

class ClickablePaintName final : public Button
{
public:
    ClickablePaintName()
        : Button(nullptr)
        , layout_(this)
    {
        this->layout_.setContentsMargins(0, 0, 0, 0);
        this->layout_.setSpacing(0);
        this->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        this->setCursor(Qt::PointingHandCursor);

        this->pixmapLabel_ = new QLabel(this);
        this->pixmapLabel_->setSizePolicy(QSizePolicy::Fixed,
                                          QSizePolicy::Fixed);
        this->pixmapLabel_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        this->pixmapLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
        this->layout_.addWidget(this->pixmapLabel_);
    }

    QLabel *pixmapLabel() const
    {
        return this->pixmapLabel_;
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        // Keep Button's reliable click handling without its hover/click wash.
    }

    void paintContent(QPainter & /*painter*/) override
    {
    }

private:
    QHBoxLayout layout_;
    QLabel *pixmapLabel_ = nullptr;
};

Label *addCopyableLabel(LayoutCreator<QHBoxLayout> box, const char *tooltip,
                        PixmapButton **copyButton = nullptr)
{
    auto label = box.emplace<Label>();
    auto button = box.emplace<PixmapButton>();
    if (copyButton != nullptr)
    {
        button.assign(copyButton);
    }
    button->setPixmap(getApp()->getThemes()->buttons.copy);
    button->setScaleIndependentSize(18, 18);
    button->setDim(DimButton::Dim::Lots);
    button->setToolTip(tooltip);
    QObject::connect(
        button.getElement(), &Button::leftClicked,
        [label = label.getElement()] {
            auto copyText = label->property("copy-text").toString();

            crossPlatformCopy(copyText.isEmpty() ? label->getText() : copyText);
        });

    return label.getElement();
};

void createUsercardStatusRow(LayoutCreator<QVBoxLayout> &vbox, QWidget **rowOut,
                             QLabel **iconOut, Label **labelOut)
{
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(8, 0, 8, 0);
    layout->setSpacing(4);

    auto *icon = new QLabel(row);
    icon->setVisible(false);
    layout->addWidget(icon, 0, Qt::AlignVCenter);

    auto *label = new Label("");
    label->setPadding({});
    layout->addWidget(label, 0, Qt::AlignVCenter);
    layout->addStretch(1);

    row->setVisible(false);
    vbox->addWidget(row);

    *rowOut = row;
    *iconOut = icon;
    *labelOut = label;
}

void createUsercardPaintRow(LayoutCreator<QVBoxLayout> &vbox, QWidget **rowOut,
                            QLabel **pixmapOut, Button **paintNameButtonOut)
{
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(8, 0, 8, 0);
    layout->setSpacing(0);

    auto *prefixLabel = new Label(QString(TEXT_SEVENTV_PAINT));
    prefixLabel->setPadding({});
    layout->addWidget(prefixLabel, 0, Qt::AlignVCenter);

    auto *paintNameButton = new ClickablePaintName;
    layout->addWidget(paintNameButton, 0, Qt::AlignVCenter);
    layout->addStretch(1);

    row->setVisible(false);
    vbox->addWidget(row);

    *rowOut = row;
    *pixmapOut = paintNameButton->pixmapLabel();
    *paintNameButtonOut = paintNameButton;
}

void createUsercardColorRow(LayoutCreator<QVBoxLayout> &vbox, QWidget **rowOut,
                            QWidget **swatchOut, Label **labelOut)
{
    auto *row = new ClickableColorRow;
    auto *layout = row->layout();
    row->setCursor(Qt::PointingHandCursor);
    row->setToolTip("Click to copy color");

    auto *swatch = new QFrame(row);
    swatch->setObjectName("UsercardColorSwatch");
    swatch->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(swatch, 0, Qt::AlignVCenter);

    auto *label = new Label("");
    label->setPadding({});
    label->setCursor(Qt::PointingHandCursor);
    label->setToolTip(row->toolTip());
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(label, 0, Qt::AlignVCenter);
    layout->addStretch(1);

    row->setVisible(false);
    vbox->addWidget(row);

    *rowOut = row;
    *swatchOut = swatch;
    *labelOut = label;
}

QPixmap renderUsercardStatusIcon(const QString &path, int size, qreal scale)
{
    static QHash<QString, QPixmap> cache;
    const auto key = QStringLiteral("%1:%2:%3").arg(path).arg(size).arg(scale);
    if (auto it = cache.find(key); it != cache.end())
    {
        return *it;
    }

    QPixmap pixmap(QSize(size, size) * scale);
    pixmap.setDevicePixelRatio(scale);
    pixmap.fill(Qt::transparent);

    QSvgRenderer renderer(path);
    QPainter painter(&pixmap);
    renderer.render(&painter, QRectF(0, 0, size, size));

    cache.insert(key, pixmap);
    return pixmap;
}

QDateTime parseIvrTimestamp(const QString &isoTimestamp)
{
    auto timestamp = QDateTime::fromString(isoTimestamp, Qt::ISODateWithMs);
    if (!timestamp.isValid())
    {
        timestamp = QDateTime::fromString(isoTimestamp, Qt::ISODate);
    }
    if (!timestamp.isValid() && isoTimestamp.contains('.'))
    {
        auto trimmed = isoTimestamp;
        const auto dotIndex = trimmed.indexOf('.');
        const auto zoneIndex = trimmed.indexOf('Z', dotIndex);
        if (zoneIndex > dotIndex + 4)
        {
            trimmed = trimmed.left(dotIndex + 4) + trimmed.mid(zoneIndex);
            timestamp = QDateTime::fromString(trimmed, Qt::ISODateWithMs);
        }
    }

    return timestamp;
}

QString formatIvrDate(const QString &isoTimestamp)
{
    const auto timestamp = parseIvrTimestamp(isoTimestamp);
    if (!timestamp.isValid())
    {
        return {};
    }

    return timestamp.toLocalTime().date().toString(Qt::ISODate);
}

int completeCalendarMonthsBetween(const QDate &from, const QDate &to)
{
    if (!from.isValid() || !to.isValid() || from > to)
    {
        return 0;
    }

    auto months = (to.year() - from.year()) * 12 + (to.month() - from.month());
    if (to.day() < from.day())
    {
        --months;
    }

    return std::max(months, 0);
}

QString formatUsercardCount(int count, const QString &unit)
{
    return QStringLiteral("%1 %2%3").arg(count).arg(unit).arg(
        count == 1 ? QString() : QStringLiteral("s"));
}

QString formatUsercardYearsMonths(int totalMonths)
{
    if (totalMonths < 12)
    {
        return {};
    }

    const auto years = totalMonths / 12;
    const auto months = totalMonths % 12;
    auto result = QStringLiteral("%1y").arg(years);
    if (months > 0)
    {
        result += QStringLiteral(" %1m").arg(months);
    }

    return QStringLiteral(" (%1)").arg(result);
}

QString formatUsercardFollowRelativeTime(const QDate &followedDate)
{
    const auto today = QDateTime::currentDateTimeUtc().date();
    if (!followedDate.isValid() || followedDate > today)
    {
        return {};
    }

    const auto months = completeCalendarMonthsBetween(followedDate, today);
    if (months >= 12)
    {
        return formatUsercardYearsMonths(months);
    }
    if (months >= 1)
    {
        return QStringLiteral(" (%1)").arg(
            formatUsercardCount(months, QStringLiteral("month")));
    }

    const auto days = followedDate.daysTo(today);
    if (days >= 14)
    {
        return QStringLiteral(" (%1)").arg(
            formatUsercardCount(days / 7, QStringLiteral("week")));
    }
    if (days > 0)
    {
        return QStringLiteral(" (%1)").arg(
            formatUsercardCount(days, QStringLiteral("day")));
    }

    return QStringLiteral(" (today)");
}

QString formatFollowButtonToolTip(const QString &displayName, bool following,
                                  const std::optional<QDateTime> &followedAt)
{
    if (!following)
    {
        return QString("Follow %1").arg(displayName);
    }

    QString tooltip = QString("Unfollow %1").arg(displayName);

    if (!followedAt || !followedAt->isValid())
    {
        return tooltip;
    }

    const auto followedDate = followedAt->date();
    const auto followingSince = followedDate.toString(Qt::ISODate);
    auto relativeTime = QString();
    if (getSettings()->showUsercardFollowageRelativeTime)
    {
        relativeTime = formatUsercardFollowRelativeTime(followedDate);
    }

    tooltip += u'\n' + QStringLiteral("Following since ") + followingSince +
               relativeTime;

    return tooltip;
}

QString formatUsercardStatus(const IvrUserProfile &profile)
{
    if (profile.isStaff)
    {
        return "Staff";
    }
    if (profile.isPartner)
    {
        return "Partner";
    }
    if (profile.isAffiliate)
    {
        return "Affiliate";
    }
    if (profile.isPreAffiliate)
    {
        return "Pre Affiliate";
    }

    return "Non Affiliate";
}

bool checkMessageUserName(const QString &userName, MessagePtr message)
{
    if (message->flags.has(MessageFlag::Whisper))
    {
        return false;
    }

    bool isSubscription = message->flags.has(MessageFlag::Subscription) &&
                          message->loginName.isEmpty() &&
                          message->messageText.split(" ").at(0).compare(
                              userName, Qt::CaseInsensitive) == 0;

    bool isModAction =
        message->timeoutUser.compare(userName, Qt::CaseInsensitive) == 0;
    bool isSelectedUser =
        message->loginName.compare(userName, Qt::CaseInsensitive) == 0;

    return (isSubscription || isModAction || isSelectedUser);
}

bool messageHasTwitchBadge(const Message &message, QStringView badge)
{
    const auto badgeName = badge.toString();
    for (const auto &twitchBadge : message.twitchBadges)
    {
        if (twitchBadge.key_.compare(badgeName, Qt::CaseInsensitive) == 0)
        {
            return true;
        }
    }

    return false;
}

ChannelPtr filterMessages(const QString &userName, ChannelPtr channel)
{
    std::vector<MessagePtr> snapshot = channel->getMessageSnapshot();

    ChannelPtr channelPtr;
    if (channel->isTwitchChannel())
    {
        channelPtr = std::make_shared<TwitchChannel>(channel->getName());
    }
    else
    {
        channelPtr =
            std::make_shared<Channel>(channel->getName(), Channel::Type::None);
    }

    for (const auto &message : snapshot)
    {
        if (checkMessageUserName(userName, message))
        {
            channelPtr->addMessage(message, MessageContext::Repost);
        }
    }

    return channelPtr;
};

QString escapeIrcTagValue(QString value)
{
    value.replace(QChar(u'\\'), QStringLiteral("\\\\"));
    value.replace(QChar(u';'), QStringLiteral("\\:"));
    value.replace(QChar(u' '), QStringLiteral("\\s"));
    value.replace(QChar(u'\r'), QStringLiteral("\\r"));
    value.replace(QChar(u'\n'), QStringLiteral("\\n"));
    return value;
}

QString cleanIrcMessageBody(QString value)
{
    value.replace(QChar(u'\r'), QChar(u' '));
    value.replace(QChar(u'\n'), QChar(u' '));
    return value;
}

MessagePtr makeUsercardModLogMessage(const GqlUsercardMessage &message,
                                     TwitchChannel *twitchChannel,
                                     const QString &channelName,
                                     const QString &fallbackUserId)
{
    auto sentAt = parseIvrTimestamp(message.sentAt);
    if (!sentAt.isValid())
    {
        sentAt = QDateTime::currentDateTime();
    }
    else
    {
        sentAt = sentAt.toLocalTime();
    }

    const auto userId =
        message.senderId.isEmpty() ? fallbackUserId : message.senderId;
    auto displayName = message.senderDisplayName.trimmed();
    if (displayName.isEmpty())
    {
        displayName = message.senderLogin;
    }
    const auto login =
        message.senderLogin.isEmpty() ? displayName : message.senderLogin;
    const auto body = cleanIrcMessageBody(message.text);

    if (twitchChannel != nullptr && !login.isEmpty())
    {
        QStringList tags;
        if (!message.id.isEmpty())
        {
            tags << QStringLiteral("id=") + escapeIrcTagValue(message.id);
        }
        if (!userId.isEmpty())
        {
            tags << QStringLiteral("user-id=") + escapeIrcTagValue(userId);
        }
        if (!message.senderColor.isEmpty())
        {
            tags << QStringLiteral("color=") +
                        escapeIrcTagValue(message.senderColor);
        }
        if (!message.senderBadges.isEmpty())
        {
            tags << QStringLiteral("badges=") +
                        escapeIrcTagValue(message.senderBadges);
        }
        if (!twitchChannel->roomId().isEmpty())
        {
            tags << QStringLiteral("room-id=") +
                        escapeIrcTagValue(twitchChannel->roomId());
        }
        if (!displayName.isEmpty())
        {
            tags << QStringLiteral("display-name=") +
                        escapeIrcTagValue(displayName);
        }
        tags << QStringLiteral("login=") + escapeIrcTagValue(login);
        if (sentAt.isValid())
        {
            tags << QStringLiteral("tmi-sent-ts=") +
                        QString::number(sentAt.toMSecsSinceEpoch());
        }

        const auto tagsText =
            tags.isEmpty() ? QString() : u"@" % tags.join(';') % u" ";
        const auto fakeIrcData =
            QStringLiteral("%1:%2!%2@%2.tmi.twitch.tv PRIVMSG #%3 :%4")
                .arg(tagsText, login, twitchChannel->getName(), body);

        auto *fakeMessage =
            Communi::IrcMessage::fromData(fakeIrcData.toUtf8(), nullptr);
        if (fakeMessage != nullptr && fakeMessage->command() == "PRIVMSG")
        {
            MessageParseArgs args;
            args.allowIgnore = false;
            auto result = MessageBuilder::makeIrcMessage(
                twitchChannel, fakeMessage, args, body, 0);
            auto builtMessage = std::move(result.first);
            fakeMessage->deleteLater();
            fakeMessage = nullptr;

            if (builtMessage)
            {
                builtMessage->flags.set(MessageFlag::DoNotLog,
                                        MessageFlag::DoNotTriggerNotification);
                if (message.isDeleted)
                {
                    builtMessage->flags.set(MessageFlag::Disabled,
                                            MessageFlag::InvalidReplyTarget);
                }
                return builtMessage;
            }
        }
        if (fakeMessage != nullptr)
        {
            fakeMessage->deleteLater();
        }
    }

    auto color = QColor(message.senderColor);
    const auto userColor = color.isValid() ? MessageColor(color)
                                           : MessageColor(MessageColor::Text);

    MessageBuilder builder;
    builder->id = message.id;
    builder->loginName = message.senderLogin;
    builder->displayName = displayName;
    builder->userID = userId;
    builder->messageText = body;
    builder->searchText = displayName + QStringLiteral(": ") + body;
    builder->channelName = channelName;
    builder->serverReceivedTime = sentAt;
    builder->usernameColor = color;
    builder->flags.set(MessageFlag::DoNotLog,
                       MessageFlag::DoNotTriggerNotification);
    if (message.isDeleted)
    {
        builder->flags.set(MessageFlag::Disabled,
                           MessageFlag::InvalidReplyTarget);
    }

    builder.emplace<TimestampElement>(sentAt.time());
    builder
        .emplace<TextElement>(displayName + QStringLiteral(":"),
                              MessageElementFlag::Username, userColor,
                              FontStyle::ChatMediumBold)
        ->setLink({Link::UserInfo, message.senderLogin});
    builder.appendOrEmplaceText(body, MessageColor::Text);

    return builder.release();
}

QDateTime oldestUsercardMessageTime(const ChannelPtr &channel)
{
    QDateTime oldest;
    if (!channel)
    {
        return oldest;
    }

    for (const auto &message : channel->getMessageSnapshot())
    {
        if (message == nullptr || !message->serverReceivedTime.isValid())
        {
            continue;
        }

        if (!oldest.isValid() || message->serverReceivedTime < oldest)
        {
            oldest = message->serverReceivedTime;
        }
    }

    return oldest;
}

qreal usercardMessagePreloadDistance(const Scrollbar &scrollbar)
{
    return std::clamp<qreal>(scrollbar.getPageSize() * 0.75, 8.0, 24.0);
}

const auto borderColor = QColor(255, 255, 255, 80);

int calculateTimeoutDuration(TimeoutButton timeout)
{
    static const QMap<QString, int> durations{
        {"s", 1}, {"m", 60}, {"h", 3600}, {"d", 86400}, {"w", 604800},
    };
    return timeout.second * durations[timeout.first];
}

QString normalizeModerationReason(QString reason)
{
    reason.replace('\r', ' ');
    reason.replace('\n', ' ');
    return reason.trimmed();
}

QString appendModerationReason(QString command, const QString &reason)
{
    const auto cleanedReason = normalizeModerationReason(reason);
    if (cleanedReason.isEmpty())
    {
        return command;
    }

    return command + ' ' + cleanedReason;
}

QString timeoutButtonReason(int index)
{
    if (index < 0)
    {
        return {};
    }

    const auto reasons = getSettings()->timeoutButtonReasons.getValue();
    if (index >= static_cast<int>(reasons.size()))
    {
        return {};
    }

    return normalizeModerationReason(reasons[index]);
}

QString timeoutBanReason()
{
    return normalizeModerationReason(
        getSettings()->timeoutBanReason.getValue());
}

bool shouldPromptForModerationReason(Qt::MouseButton button)
{
    if (button == Qt::RightButton &&
        getSettings()->timeoutReasonPromptOnRightClick.getValue())
    {
        return true;
    }

    if (!getSettings()->timeoutReasonPromptOnModifier.getValue())
    {
        return false;
    }

    const auto configuredModifier =
        getSettings()->timeoutReasonPromptModifier.getValue();
    const auto modifiers = QGuiApplication::keyboardModifiers();
    if (configuredModifier == "Ctrl")
    {
        return modifiers.testFlag(Qt::ControlModifier);
    }
    if (configuredModifier == "Alt")
    {
        return modifiers.testFlag(Qt::AltModifier);
    }

    return modifiers.testFlag(Qt::ShiftModifier);
}

bool shouldHandleModerationButtonClick(Qt::MouseButton button)
{
    return button == Qt::LeftButton ||
           (button == Qt::RightButton &&
            getSettings()->timeoutReasonPromptOnRightClick.getValue());
}

class ModerationReasonPopup final : public DraggablePopup
{
public:
    ModerationReasonPopup(const QString &title, const QString &placeholder,
                          const QString &initialReason, bool showSendButton,
                          std::function<void(QString)> onSend,
                          QWidget *parent = nullptr)
        : DraggablePopup(true, parent)
        , showSendButton_(showSendButton)
        , onSend_(std::move(onSend))
    {
        this->setWindowTitle(title);
        this->setAttribute(Qt::WA_DeleteOnClose);
        this->setScaleIndependentSize(showSendButton ? QSize(360, 42)
                                                     : QSize(290, 42));

        auto layout = LayoutCreator<QWidget>(this->getLayoutContainer())
                          .setLayoutType<QHBoxLayout>();
        this->layout_ = layout.getElement();

        this->input_ = layout.emplace<QLineEdit>().getElement();
        this->input_->setPlaceholderText(placeholder);
        this->input_->setText(initialReason);
        this->input_->setMouseTracking(true);
        this->input_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        if (this->showSendButton_)
        {
            this->sendButton_ =
                layout.emplace<QPushButton>("Send").getElement();
            this->sendButton_->setSizePolicy(QSizePolicy::Fixed,
                                             QSizePolicy::Fixed);
            this->sendButton_->setCursor(Qt::PointingHandCursor);
            this->sendButton_->setMouseTracking(true);

            QObject::connect(this->sendButton_, &QPushButton::clicked, this,
                             [this] {
                                 this->send();
                             });
        }
        QObject::connect(this->input_, &QLineEdit::returnPressed, this, [this] {
            this->send();
        });

        this->applyScaledLayout();
    }

    void showCenteredAt(const QPoint &center)
    {
        this->show();
        this->moveTo(center - QPoint(this->width() / 2, this->height() / 2),
                     widgets::BoundsChecking::DesiredPosition);
    }

protected:
    void scaleChangedEvent(float scale) override
    {
        DraggablePopup::scaleChangedEvent(scale);
        this->applyScaledLayout();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape)
        {
            QTimer::singleShot(0, this, &QWidget::close);
            return;
        }

        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            (!event->modifiers() ||
             event->modifiers().testFlag(Qt::KeypadModifier)))
        {
            this->send();
            return;
        }

        DraggablePopup::keyPressEvent(event);
    }

    void showEvent(QShowEvent *event) override
    {
        DraggablePopup::showEvent(event);

        this->input_->setFocus(Qt::PopupFocusReason);
        this->input_->selectAll();
    }

private:
    void applyScaledLayout()
    {
        const auto effectiveScale = std::max(0.75F, this->scale());
        const int marginX = std::max(4, int(6 * effectiveScale));
        const int marginY = std::max(2, int(4 * effectiveScale));
        const int spacing = std::max(4, int(6 * effectiveScale));
        const int controlHeight = std::max(22, int(24 * effectiveScale));

        this->layout_->setContentsMargins(marginX, marginY, marginX, marginY);
        this->layout_->setSpacing(spacing);

        const auto uiFont =
            getApp()->getFonts()->getFont(FontStyle::UiMedium, effectiveScale);
        const auto buttonFont = getApp()->getFonts()->getFont(
            FontStyle::UiMediumBold, effectiveScale);
        this->input_->setFont(uiFont);
        this->input_->setFixedHeight(controlHeight);
        if (this->sendButton_ != nullptr)
        {
            this->sendButton_->setFont(buttonFont);
            const QFontMetrics buttonMetrics(buttonFont);
            this->sendButton_->setFixedHeight(controlHeight);
            this->sendButton_->setMinimumWidth(
                buttonMetrics.horizontalAdvance("Send") +
                std::max(22, int(24 * effectiveScale)));
        }
    }

    void send()
    {
        if (this->sent_)
        {
            return;
        }
        this->sent_ = true;

        if (this->onSend_)
        {
            this->onSend_(this->input_->text());
        }
        QTimer::singleShot(0, this, &QWidget::close);
    }

    QHBoxLayout *layout_{};
    QLineEdit *input_{};
    QPushButton *sendButton_{};
    bool showSendButton_ = false;
    std::function<void(QString)> onSend_;
    bool sent_ = false;
};

QString hashUrl(const QString &url)
{
    QByteArray bytes;

    bytes.append(url.toUtf8());
    QByteArray hashBytes(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));

    return hashBytes.toHex();
}

constexpr qint64 FOLLOWING_STATUS_RETRY_INTERVAL_MS = 30'000;

QMutex &activeUsercardsMutex()
{
    static QMutex mutex;
    return mutex;
}

QList<QPointer<UserInfoPopup>> &activeUsercards()
{
    static QList<QPointer<UserInfoPopup>> cards;
    return cards;
}

void registerActiveUsercard(UserInfoPopup *popup)
{
    QMutexLocker locker(&activeUsercardsMutex());
    activeUsercards().append(popup);
}

void unregisterActiveUsercard(UserInfoPopup *popup)
{
    QMutexLocker locker(&activeUsercardsMutex());
    activeUsercards().removeAll(popup);
}

}  // namespace

namespace chatterino {

using namespace literals;

UserInfoPopup::UserInfoPopup(bool closeAutomatically, Split *split)
    : DraggablePopup(closeAutomatically, split)
    , split_(split)
    , closeAutomatically_(closeAutomatically)
{
    registerActiveUsercard(this);

    this->followingStatusChangedConnection_ =
        std::make_unique<pajlada::Signals::ScopedConnection>(
            this->followingStatusChanged_.connect([this] {
                this->updateUsercardFollowButton();
            }));

    assert(split != nullptr &&
           "split being nullptr causes lots of bugs down the road");
    this->setWindowTitle("Usercard");

    HotkeyController::HotkeyMap actions{
        {"delete",
         [this](std::vector<QString>) -> QString {
             this->deleteLater();
             return "";
         }},
        {"scrollPage",
         [this](std::vector<QString> arguments) -> QString {
             if (arguments.size() == 0)
             {
                 qCWarning(chatterinoHotkeys)
                     << "scrollPage hotkey called without arguments!";
                 return "scrollPage hotkey called without arguments!";
             }
             auto direction = arguments.at(0);

             auto &scrollbar = this->ui_.latestMessages->getScrollBar();
             if (direction == "up")
             {
                 scrollbar.offset(-scrollbar.getPageSize());
             }
             else if (direction == "down")
             {
                 scrollbar.offset(scrollbar.getPageSize());
             }
             else
             {
                 qCWarning(chatterinoHotkeys) << "Unknown scroll direction";
             }
             return "";
         }},
        {"execModeratorAction",
         [this](std::vector<QString> arguments) -> QString {
             if (!this->shouldShowModerationActions())
             {
                 return "";
             }

             if (arguments.empty())
             {
                 return "execModeratorAction action needs an argument, which "
                        "moderation action to execute, see description in the "
                        "editor";
             }
             auto target = arguments.at(0);
             UsercardModerationRequest request;

             // these can't have /timeout/ buttons because they are not timeouts
             if (target == "ban")
             {
                 request.action = UsercardModerationAction::Ban;
                 request.reason = timeoutBanReason();
             }
             else if (target == "unban")
             {
                 request.action = UsercardModerationAction::Unban;
             }
             else
             {
                 // find and execute timeout button #TARGET

                 bool ok;
                 int buttonNum = target.toInt(&ok);
                 if (!ok)
                 {
                     return QString("Invalid argument for execModeratorAction: "
                                    "%1. Use "
                                    "\"ban\", \"unban\" or the number of the "
                                    "timeout "
                                    "button to execute")
                         .arg(target);
                 }

                 const auto &timeoutButtons =
                     getSettings()->timeoutButtons.getValue();
                 if (static_cast<int>(timeoutButtons.size()) < buttonNum ||
                     0 >= buttonNum)
                 {
                     return QString("Invalid argument for execModeratorAction: "
                                    "%1. Integer out of usable range: [1, %2]")
                         .arg(buttonNum,
                              static_cast<int>(timeoutButtons.size()));
                 }
                 const auto &button = timeoutButtons.at(buttonNum - 1);
                 request.action = UsercardModerationAction::Timeout;
                 request.durationSeconds = calculateTimeoutDuration(button);
                 request.reason = timeoutButtonReason(buttonNum - 1);
             }

             this->executeUsercardModerationAction(request);
             return "";
         }},
        {"pin",
         [this](std::vector<QString> /*arguments*/) -> QString {
             this->togglePinned();
             return "";
         }},
        {"openProfilePictureMenu",
         [this](std::vector<QString> /*arguments*/) -> QString {
             return this->showProfilePictureContextMenu();
         }},

        // these actions make no sense in the context of a usercard, so they aren't implemented
        {"reject", nullptr},
        {"accept", nullptr},
        {"openTab", nullptr},
        {"search", nullptr},
    };

    this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
        HotkeyCategory::PopupWindow, actions, this);

    auto layers = LayoutCreator<QWidget>(this->getLayoutContainer())
                      .setLayoutType<QGridLayout>()
                      .withoutMargin();
    auto layout = layers.emplace<QVBoxLayout>();

    // first line
    auto head = layout.emplace<QHBoxLayout>().withoutMargin();
    {
        auto avatarBox = head.emplace<QVBoxLayout>().withoutMargin();
        avatarBox->setAlignment(Qt::AlignTop);
        // avatar
        auto *avatarFrame = new QWidget(this);
        auto *avatarLayout = new QGridLayout(avatarFrame);
        avatarLayout->setContentsMargins(0, 0, 0, 0);
        avatarLayout->setSpacing(0);
        avatarBox->addWidget(avatarFrame);

        auto *avatar = new PixmapButton(nullptr);
        this->ui_.avatarButton = avatar;
        avatar->setScaleIndependentSize(100, 100);
        avatar->setDim(DimButton::Dim::None);
        avatarLayout->addWidget(avatar, 0, 0);
        QObject::connect(avatar, &Button::clicked,
                         [this](Qt::MouseButton button) {
                             if (this->isKick_)
                             {
                                 this->onKickProfilePictureClick(button);
                                 return;
                             }

                             QUrl channelURL("https://www.twitch.tv/" +
                                             this->userName_.toLower());

                             switch (button)
                             {
                                 case Qt::LeftButton: {
                                     QDesktopServices::openUrl(channelURL);
                                 }
                                 break;

                                 case Qt::RightButton: {
                                     if (this->avatarUrl_.isEmpty())
                                     {
                                         return;
                                     }
                                     this->showProfilePictureContextMenu();
                                 }
                                 break;

                                 default:;
                             }
                         });
        auto *bannedLabel = new QLabel("BANNED", avatarFrame);
        bannedLabel->setAlignment(Qt::AlignCenter);
        bannedLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        bannedLabel->setStyleSheet("QLabel { background: rgba(185, 28, 28, "
                                   "220); color: white; font-weight: 700; "
                                   "padding: 2px 6px; border-radius: 3px; }");
        bannedLabel->hide();
        avatarLayout->addWidget(bannedLabel, 0, 0,
                                Qt::AlignHCenter | Qt::AlignBottom);
        this->ui_.bannedAvatarLabel = bannedLabel;

        auto switchAv =
            avatarBox.emplace<LabelButton>(QString{}, nullptr, QSize{2, 2})
                .assign(&this->ui_.switchAvatars);
        switchAv->hide();
        QObject::connect(
            switchAv.getElement(), &LabelButton::leftClicked, [this] {
                if (!this->seventvAvatar_)
                {
                    this->ui_.switchAvatars->hide();
                    return;
                }
                this->isTwitchAvatarShown_ = !this->isTwitchAvatarShown_;
                if (this->isTwitchAvatarShown_)
                {
                    this->seventvAvatar_->stop();
                    this->ui_.avatarButton->setPixmap(this->avatarPixmap_);
                    this->ui_.switchAvatars->setText("Show 7TV");
                }
                else
                {
                    this->ui_.avatarButton->setPixmap(
                        this->seventvAvatar_->currentPixmap());
                    this->seventvAvatar_->start();
                    this->ui_.switchAvatars->setText(u"Show " %
                                                     this->platformName());
                }
                this->updateAvatarUrl();
            });

        avatarBox->addSpacing(2);
        auto *followButton =
            new SvgButton(followButtonSource(false), this, {4, 4});
        followButton->hide();
        this->ui_.followButton = followButton;
        avatarBox->addWidget(followButton, 0, Qt::AlignHCenter);
        QObject::connect(followButton, &Button::leftClicked, this, [this] {
            this->toggleUsercardFollow();
        });

        auto vbox = head.emplace<QVBoxLayout>();
        {
            // items on the right
            {
                auto box = vbox.emplace<QHBoxLayout>()
                               .withoutMargin()
                               .withoutSpacing();

                this->ui_.nameLabel = addCopyableLabel(box, "Copy name");
                this->ui_.nameLabel->setFontStyle(FontStyle::UiMediumBold);
                this->ui_.nameLabel->setPadding(QMargins(8, 0, 1, 0));
                this->ui_.liveIndicator = new LiveIndicator;
                this->ui_.liveIndicator->hide();
                // addCopyableLabel adds the copy button last -> add the indicator before that
                box->insertWidget(box->count() - 1, this->ui_.liveIndicator);
                box->insertItem(box->count() - 1,
                                ScalingSpacerItem::horizontal(7));
                auto nameHistory =
                    box.emplace<LabelButton>("aka", this, QSize{4, 0})
                        .assign(&this->ui_.nameHistoryButton);
                nameHistory->setToolTip("Show name history");
                nameHistory->hide();
                QObject::connect(nameHistory.getElement(), &Button::leftClicked,
                                 [this] {
                                     this->showNameHistoryMenu();
                                 });
                box->addSpacing(5);
                box->addStretch(1);

                this->ui_.localizedNameLabel =
                    addCopyableLabel(box, "Copy localized name",
                                     &this->ui_.localizedNameCopyButton);
                this->ui_.localizedNameLabel->setFontStyle(
                    FontStyle::UiMediumBold);
                box->addSpacing(5);
                box->addStretch(1);

                auto palette = QPalette();
                palette.setColor(QPalette::WindowText, QColor("#aaa"));
                this->ui_.userIDLabel = addCopyableLabel(box, "Copy ID");
                this->ui_.userIDLabel->setPalette(palette);

                this->ui_.localizedNameLabel->setVisible(false);
                this->ui_.localizedNameCopyButton->setVisible(false);

                // button to pin the window (only if we close automatically)
                if (this->closeAutomatically_)
                {
                    box->addWidget(this->createPinButton());
                }

                QPointer<UserInfoPopup> self(this);
                this->currentUserChangedConnection_ =
                    getApp()->getAccounts()->twitch.currentUserChanged.connect(
                        [self] {
                            runInGuiThread([self] {
                                if (!self)
                                {
                                    return;
                                }

                                if (!self->isKick_ && self->underlyingChannel_)
                                {
                                    if (auto *twitchChannel =
                                            dynamic_cast<TwitchChannel *>(
                                                self->underlyingChannel_.get()))
                                    {
                                        twitchChannel->refreshLeadModStatus();
                                    }
                                }

                                if (!self->isKick_ &&
                                    (!self->userName_.isEmpty() ||
                                     !self->userId_.isEmpty()))
                                {
                                    self->resetUsercardInfoRows();
                                    self->updateUserData();
                                }

                                if (!self->isKick_ &&
                                    !self->userId_.isEmpty() &&
                                    getSettings()->showFollowButtonInUsercard)
                                {
                                    self->refreshFollowingStatus(true);
                                }
                                self->updateUsercardFollowButton();
                                self->userStateChanged_.invoke();
                            });
                        });
            }

            // items on the left
            if (getSettings()->showPronouns)
            {
                vbox.emplace<Label>(TEXT_PRONOUNS.arg(TEXT_LOADING))
                    .assign(&this->ui_.pronounsLabel);
            }
            vbox.emplace<Label>(TEXT_FOLLOWERS.arg(""))
                .assign(&this->ui_.followerCountLabel);
            vbox.emplace<Label>(TEXT_CREATED.arg(""))
                .assign(&this->ui_.createdDateLabel);
            vbox.emplace<Label>("").assign(&this->ui_.lastLiveLabel);
            createUsercardColorRow(vbox, &this->ui_.userColorRow,
                                   &this->ui_.userColorSwatch,
                                   &this->ui_.userColorLabel);
            if (auto *colorRow =
                    dynamic_cast<ClickableColorRow *>(this->ui_.userColorRow))
            {
                QObject::connect(colorRow, &Button::leftClicked, this, [this] {
                    const auto color =
                        this->ui_.userColorRow->property("copy-color")
                            .toString();
                    if (color.isEmpty())
                    {
                        return;
                    }

                    crossPlatformCopy(color);
                    const auto message =
                        QString("Copied user color %1").arg(color);
                    QToolTip::showText(QCursor::pos(), message, this);
                    if (this->channel_)
                    {
                        this->channel_->addSystemMessage(message);
                    }
                });
            }
            Button *seventvPaintNameButton = nullptr;
            createUsercardPaintRow(vbox, &this->ui_.seventvPaintRow,
                                   &this->ui_.seventvPaintPixmapLabel,
                                   &seventvPaintNameButton);
            if (seventvPaintNameButton)
            {
                QObject::connect(
                    seventvPaintNameButton, &Button::leftClicked, this, [this] {
                        if (!this->seventvPaint_ ||
                            this->seventvPaint_->id.isEmpty())
                        {
                            return;
                        }

                        QDesktopServices::openUrl(QUrl(
                            SUSGEE_PAINT_PAGE.arg(this->seventvPaint_->id)));
                    });
            }
            vbox.emplace<Label>("").assign(&this->ui_.statusLabel);
            vbox.emplace<Label>("").assign(&this->ui_.chatterCountLabel);
            createUsercardStatusRow(vbox, &this->ui_.followageRow,
                                    &this->ui_.followageIcon,
                                    &this->ui_.followageLabel);
            createUsercardStatusRow(vbox, &this->ui_.subageRow,
                                    &this->ui_.subageIcon,
                                    &this->ui_.subageLabel);
            createUsercardStatusRow(vbox, &this->ui_.subGiftRow,
                                    &this->ui_.subGiftIcon,
                                    &this->ui_.subGiftLabel);

            auto applyPopupVisibility = [this] {
                auto *settings = getSettings();
                if (this->ui_.chatterCountLabel)
                {
                    this->ui_.chatterCountLabel->setVisible(
                        settings->showUsercardChatterCount &&
                        settings->showUserinfoPopupChatters.getValue());
                }
                if (this->ui_.lastLiveLabel)
                {
                    this->ui_.lastLiveLabel->setVisible(
                        settings->showUsercardLastLive &&
                        settings->showUserinfoPopupLastLive.getValue());
                }
                if (this->ui_.userColorRow)
                {
                    this->ui_.userColorRow->setVisible(
                        settings->showUsercardColor &&
                        settings->showUserinfoPopupColor.getValue());
                }
            };

            applyPopupVisibility();
            getSettings()->showUserinfoPopupChatters.connect(
                [applyPopupVisibility](auto) {
                    applyPopupVisibility();
                },
                this->signalHolder_, false);
            getSettings()->showUserinfoPopupLastLive.connect(
                [applyPopupVisibility](auto) {
                    applyPopupVisibility();
                },
                this->signalHolder_, false);
            getSettings()->showUserinfoPopupColor.connect(
                [applyPopupVisibility](auto) {
                    applyPopupVisibility();
                },
                this->signalHolder_, false);
        }
    }

    layout.emplace<Line>(false);

    // second line
    auto user = layout.emplace<QHBoxLayout>().withoutMargin();
    {
        user->addStretch(1);

        user.emplace<QCheckBox>("Block").assign(&this->ui_.block);
        user.emplace<QCheckBox>("Ignore highlights")
            .assign(&this->ui_.ignoreHighlights);
        // visibility of this is updated in setData

        user.emplace<LabelButton>("Add &notes", this)
            .assign(&this->ui_.notesAdd);
        auto usercard = user.emplace<LabelButton>("&Usercard", this)
                            .assign(&this->ui_.usercardLabel);
        auto userlogs = user.emplace<LabelButton>("&Logs", this)
                            .assign(&this->ui_.userlogsLabel);
        userlogs->hide();
        auto sevenTVUser = user.emplace<LabelButton>("7TV", this)
                               .assign(&this->ui_.sevenTVUserLabel);
        sevenTVUser->setToolTip("Checking 7TV profile...");
        sevenTVUser->setEnabled(false);
        sevenTVUser->hide();
        auto roles = user.emplace<LabelButton>("Roles", this)
                         .assign(&this->ui_.rolesLabel);
        roles->setToolTip("Manage editor and lead mod roles");
        roles->hide();
        auto mod = user.emplace<PixmapButton>(this);
        mod->setPixmap(getResources().buttons.mod);
        mod->setScaleIndependentSize(30, 30);
        auto unmod = user.emplace<PixmapButton>(this);
        unmod->setPixmap(getResources().buttons.unmod);
        unmod->setScaleIndependentSize(30, 30);
        auto vip = user.emplace<PixmapButton>(this);
        vip->setPixmap(getResources().buttons.vip);
        vip->setScaleIndependentSize(30, 30);
        auto unvip = user.emplace<PixmapButton>(this);
        unvip->setPixmap(getResources().buttons.unvip);
        unvip->setScaleIndependentSize(30, 30);

        user->addStretch(1);

        auto openUsercard = [this] {
            if (!this->underlyingChannel_)
            {
                return;
            }

            QDesktopServices::openUrl("https://www.twitch.tv/popout/" +
                                      this->underlyingChannel_->getName() +
                                      "/viewercard/" + this->userName_);
        };
        QObject::connect(usercard.getElement(), &Button::leftClicked,
                         openUsercard);
        this->registerMnemonicButton(this->ui_.usercardLabel, Qt::Key_U,
                                     openUsercard);

        auto openLogs = [this] {
            if (!this->underlyingChannel_)
            {
                return;
            }

            QUrl url("https://tv.supa.sh/logs");
            QUrlQuery query;
            query.addQueryItem("c", this->underlyingChannel_->getName());
            query.addQueryItem("u", this->userName_);
            url.setQuery(query);
            QDesktopServices::openUrl(url);
        };
        QObject::connect(userlogs.getElement(), &Button::leftClicked, openLogs);
        this->registerMnemonicButton(this->ui_.userlogsLabel, Qt::Key_L,
                                     openLogs);

        auto openSevenTVUser = [this] {
            if (this->seventvUserID_.isEmpty())
            {
                this->refreshSevenTVUserButtonVisibility();
                return;
            }

            QDesktopServices::openUrl(
                QUrl(SEVENTV_USER_PAGE % this->seventvUserID_));
        };
        QObject::connect(sevenTVUser.getElement(), &Button::leftClicked,
                         openSevenTVUser);
        this->registerMnemonicButton(this->ui_.sevenTVUserLabel, Qt::Key_V,
                                     openSevenTVUser);

        auto openRoleMenu = [this] {
            this->showRoleManagementMenu();
        };
        QObject::connect(roles.getElement(), &Button::leftClicked,
                         openRoleMenu);
        this->registerMnemonicButton(this->ui_.rolesLabel, Qt::Key_R,
                                     openRoleMenu);

        QObject::connect(mod.getElement(), &Button::leftClicked, [this] {
            QString value = "/mod " + this->userName_;
            value = getApp()->getCommands()->execCommand(
                value, this->underlyingChannel_, false);
            this->underlyingChannel_->sendMessage(value);
        });
        QObject::connect(unmod.getElement(), &Button::leftClicked, [this] {
            QString value = "/unmod " + this->userName_;
            value = getApp()->getCommands()->execCommand(
                value, this->underlyingChannel_, false);
            this->underlyingChannel_->sendMessage(value);
        });
        QObject::connect(vip.getElement(), &Button::leftClicked, [this] {
            QString value = "/vip " + this->userName_;
            value = getApp()->getCommands()->execCommand(
                value, this->underlyingChannel_, false);
            this->underlyingChannel_->sendMessage(value);
        });
        QObject::connect(unvip.getElement(), &Button::leftClicked, [this] {
            QString value = "/unvip " + this->userName_;
            value = getApp()->getCommands()->execCommand(
                value, this->underlyingChannel_, false);
            this->underlyingChannel_->sendMessage(value);
        });

        // userstate
        // We can safely ignore this signal connection since this is a private signal, and
        // we only connect once
        std::ignore = this->userStateChanged_.connect([this, mod, unmod, vip,
                                                       unvip, roles]() mutable {
            TwitchChannel *twitchChannel =
                dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get());

            bool visibilityModButtons = false;

            if (twitchChannel)
            {
                bool isMyself =
                    QString::compare(getApp()
                                         ->getAccounts()
                                         ->twitch.getCurrent()
                                         ->getUserName(),
                                     this->userName_, Qt::CaseInsensitive) == 0;

                const bool canManageRoles =
                    twitchChannel->isBroadcaster() ||
                    (getSettings()->showLeadModRoleButtons &&
                     twitchChannel->isLeadMod());

                visibilityModButtons =
                    canManageRoles && !isMyself && !this->isBroadcaster_;
            }
            mod->setVisible(visibilityModButtons);
            unmod->setVisible(visibilityModButtons);
            vip->setVisible(visibilityModButtons);
            unvip->setVisible(visibilityModButtons);
            roles->setVisible(this->canShowRoleManagementMenu());
        });
    }

    auto notesPreview = layout.emplace<MarkdownLabel>(this, QString())
                            .assign(&this->ui_.notesPreview);
    notesPreview->setVisible(false);
    notesPreview->setShouldElide(true);

    auto lineMod = layout.emplace<Line>(false);

    // third line
    auto moderation = layout.emplace<QHBoxLayout>().withoutMargin();
    {
        auto timeout = moderation.emplace<TimeoutWidget>().assign(
            &this->ui_.timeoutWidget);

        // We can safely ignore this signal connection since this is a private signal, and
        // we only connect once
        std::ignore =
            this->userStateChanged_.connect([this, lineMod, timeout]() mutable {
                bool visible = this->shouldShowModerationActions();
                lineMod->setVisible(visible);
                timeout->setVisible(visible);
            });

        // We can safely ignore this signal connection since we own the button, and
        // the button will always be destroyed before the UserInfoPopup
        std::ignore = timeout->buttonClicked.connect(
            [this](const UsercardModerationRequest &request) {
                if (request.promptForReason)
                {
                    this->showUsercardModerationReasonPopup(request);
                    return;
                }

                this->executeUsercardModerationAction(request);
            });
    }

    layout.emplace<Line>(false);

    // fourth line (last messages)
    auto logs = layout.emplace<QVBoxLayout>().withoutMargin();
    {
        this->ui_.noMessagesLabel = new Label("No recent messages");
        this->ui_.noMessagesLabel->setVisible(false);
        this->ui_.noMessagesLabel->setSizePolicy(QSizePolicy::Expanding,
                                                 QSizePolicy::Expanding);

        this->ui_.latestMessages =
            new ChannelView(this, this->split_, ChannelView::Context::UserCard,
                            getSettings()->scrollbackUsercardLimit);
        this->ui_.latestMessages->setMinimumSize(400, 275);
        this->ui_.latestMessages->setSizePolicy(QSizePolicy::Expanding,
                                                QSizePolicy::Expanding);

        auto loadMore =
            new LabelButton("Load more messages", this, QSize{8, 2});
        loadMore->setVisible(false);
        loadMore->setToolTip("Load older messages from Twitch mod logs");
        this->ui_.loadMoreMessages = loadMore;

        QObject::connect(loadMore, &Button::leftClicked, this, [this] {
            this->requestMoreUsercardMessages(true);
        });
        this->usercardScrollConnection_ =
            std::make_unique<pajlada::Signals::ScopedConnection>(
                this->ui_.latestMessages->getScrollBar()
                    .getDesiredValueChanged()
                    .connect([this] {
                        this->maybeLoadMoreUsercardMessagesFromScroll();
                    }));

        logs->addWidget(this->ui_.loadMoreMessages);
        logs->addWidget(this->ui_.noMessagesLabel);
        logs->addWidget(this->ui_.latestMessages);
        logs->setAlignment(this->ui_.noMessagesLabel, Qt::AlignHCenter);
        logs->setAlignment(this->ui_.loadMoreMessages, Qt::AlignHCenter);
    }

    // size grip
    if (closeAutomatically)
    {
        layers->addWidget(new InvisibleSizeGrip(this), 0, 0,
                          Qt::AlignRight | Qt::AlignBottom);
    }

    this->installEvents();
    this->updateUsercardStatusIcons();
    std::ignore = this->userStateChanged_.connect([this] {
        this->updateLoadMoreMessagesButton();
    });
    this->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Policy::Ignored);
}

void UserInfoPopup::themeChangedEvent()
{
    BaseWindow::themeChangedEvent();

    for (auto &&child : this->findChildren<QCheckBox *>())
    {
        child->setFont(
            getApp()->getFonts()->getFont(FontStyle::UiMedium, this->scale()));
    }

    this->updateUsercardStatusIcons();

    if (this->seventvPaint_)
    {
        this->updateSeventvPaintPixmap();
    }
}

void UserInfoPopup::scaleChangedEvent(float scale)
{
    this->themeChangedEvent();

    if (this->ui_.followButton != nullptr)
    {
        const int buttonSize = std::max(1, int(28 * scale));
        this->ui_.followButton->setFixedSize(buttonSize, buttonSize);
    }

    QTimer::singleShot(20, this, [this] {
        auto geo = this->geometry();
        geo.setWidth(10);
        geo.setHeight(10);

        this->setGeometry(geo);
    });
}

void UserInfoPopup::windowDeactivationEvent()
{
    if (this->editUserNotesDialog_.isNull() ||
        !this->editUserNotesDialog_->isVisible())
    {
        BaseWindow::windowDeactivationEvent();
    }
}

void UserInfoPopup::registerMnemonicButton(LabelButton *button, int key,
                                           std::function<void()> action)
{
    if (button == nullptr)
    {
        return;
    }

    this->mnemonicActions_[key] = {
        std::move(action),
        [button] {
            return button->isVisible() && button->isEnabled();
        },
    };
}

void UserInfoPopup::keyPressEvent(QKeyEvent *event)
{
    const auto modifiers = event->modifiers() & ~Qt::KeypadModifier;
    if (modifiers == Qt::NoModifier || modifiers == Qt::AltModifier)
    {
        auto it = this->mnemonicActions_.find(event->key());
        if (it != this->mnemonicActions_.end())
        {
            const auto &[action, canRun] = it->second;
            if (!canRun || canRun())
            {
                action();
                event->accept();
                return;
            }
        }
    }

    DraggablePopup::keyPressEvent(event);
}

void UserInfoPopup::installEvents()
{
    std::shared_ptr<bool> ignoreNext = std::make_shared<bool>(false);

    // block
    QObject::connect(
        this->ui_.block, &QCheckBox::stateChanged,
        [this](int newState) mutable {
            if (this->isKick_)
            {
                return;
            }

            auto currentUser = getApp()->getAccounts()->twitch.getCurrent();

            const auto reenableBlockCheckbox = [this] {
                this->ui_.block->setEnabled(true);
            };

            if (!this->ui_.block->isEnabled())
            {
                reenableBlockCheckbox();
                return;
            }

            if (newState == Qt::Unchecked)
            {
                this->ui_.block->setEnabled(false);

                getApp()->getAccounts()->twitch.getCurrent()->unblockUser(
                    this->userId_, this->userName_, this,
                    [this, reenableBlockCheckbox, currentUser] {
                        this->channel_->addSystemMessage(
                            QString("You successfully unblocked user %1")
                                .arg(this->userName_));
                        reenableBlockCheckbox();
                    },
                    [this, reenableBlockCheckbox] {
                        this->channel_->addSystemMessage(
                            QString("User %1 couldn't be unblocked, an unknown "
                                    "error occurred!")
                                .arg(this->userName_));
                        reenableBlockCheckbox();
                    });
                return;
            }

            if (newState == Qt::Checked)
            {
                this->ui_.block->setEnabled(false);

                bool wasPinned = this->ensurePinned();
                auto btn = QMessageBox::warning(
                    this, u"Blocking " % this->userName_,
                    u"Blocking %1 can cause unintended side-effects like unfollowing.\n\n"_s
                    "Are you sure you want to block %1?".arg(this->userName_),
                    QMessageBox::Yes | QMessageBox::Cancel,
                    QMessageBox::Cancel);
                if (wasPinned)
                {
                    this->togglePinned();
                }
                if (btn != QMessageBox::Yes)
                {
                    reenableBlockCheckbox();
                    QSignalBlocker blocker(this->ui_.block);
                    this->ui_.block->setCheckState(Qt::Unchecked);
                    return;
                }

                getApp()->getAccounts()->twitch.getCurrent()->blockUser(
                    this->userId_, this->userName_, this,
                    [this, reenableBlockCheckbox, currentUser] {
                        this->channel_->addSystemMessage(
                            QString("You successfully blocked user %1")
                                .arg(this->userName_));
                        reenableBlockCheckbox();
                    },
                    [this, reenableBlockCheckbox] {
                        this->channel_->addSystemMessage(
                            QString("User %1 couldn't be blocked, an "
                                    "unknown error occurred!")
                                .arg(this->userName_));
                        reenableBlockCheckbox();
                    });
                return;
            }

            qCWarning(chatterinoWidget)
                << "Unexpected check-state when blocking" << this->userName_
                << QMetaEnum::fromType<Qt::CheckState>().valueToKey(newState);
        });

    // ignore highlights
    QObject::connect(
        this->ui_.ignoreHighlights, &QCheckBox::clicked,
        [this](bool checked) mutable {
            this->ui_.ignoreHighlights->setEnabled(false);

            if (checked)
            {
                getSettings()->blacklistedUsers.insert(
                    HighlightBlacklistUser{this->userName_, false});
                this->ui_.ignoreHighlights->setEnabled(true);
            }
            else
            {
                const auto &vector = getSettings()->blacklistedUsers.raw();

                for (int i = 0; i < static_cast<int>(vector.size()); i++)
                {
                    if (this->userName_ ==
                        vector[static_cast<size_t>(i)].getPattern())
                    {
                        getSettings()->blacklistedUsers.removeAt(i);
                        i--;
                    }
                }
                if (getSettings()->isBlacklistedUser(this->userName_))
                {
                    this->ui_.ignoreHighlights->setToolTip(
                        "Name matched by regex");
                }
                else
                {
                    this->ui_.ignoreHighlights->setEnabled(true);
                }
            }
        });

    // user notes
    auto openNotes = [this]() mutable {
        if (this->editUserNotesDialog_.isNull())
        {
            this->editUserNotesDialog_ = new EditUserNotesDialog(this);
            // ignoring since it the dialog is only used in this instance
            std::ignore = this->editUserNotesDialog_->onOk.connect(
                [userId = this->userId_](const QString &newNotes) {
                    getApp()->getUserData()->setUserNotes(userId, newNotes);
                });
        }

        auto userData = getApp()->getUserData()->getUser(this->userId_);
        auto initialNotes = userData.has_value() ? userData->notes : QString();

        this->editUserNotesDialog_->setNotes(initialNotes);
        this->editUserNotesDialog_->updateWindowTitle(this->userName_);
        this->editUserNotesDialog_->show();
    };
    QObject::connect(this->ui_.notesAdd, &LabelButton::clicked,
                     [openNotes](Qt::MouseButton) mutable {
                         openNotes();
                     });
    this->registerMnemonicButton(this->ui_.notesAdd, Qt::Key_N, openNotes);

    this->userDataUpdatedConnection_ =
        std::make_unique<pajlada::Signals::ScopedConnection>(
            getApp()->getUserData()->userDataUpdated().connect([this]() {
                this->updateNotes();
            }));

    getSettings()->hideModActionsOnModUsercards.connect(
        [this](bool enabled) {
            if (enabled && getSettings()->showModActionsOnModUsercardsAsLeadMod)
            {
                if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(
                        this->underlyingChannel_.get()))
                {
                    twitchChannel->refreshLeadModStatus();
                }
            }
            this->userStateChanged_.invoke();
        },
        this->signalHolder_);
    getSettings()->showModActionsOnModUsercardsAsLeadMod.connect(
        [this](bool enabled) {
            if (enabled && getSettings()->hideModActionsOnModUsercards)
            {
                if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(
                        this->underlyingChannel_.get()))
                {
                    twitchChannel->refreshLeadModStatus();
                }
            }

            this->userStateChanged_.invoke();
        },
        this->signalHolder_);
    getSettings()->showLeadModRoleButtons.connect(
        [this](bool enabled) {
            if (enabled)
            {
                if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(
                        this->underlyingChannel_.get()))
                {
                    twitchChannel->refreshLeadModStatus(true);
                }
            }

            this->userStateChanged_.invoke();
        },
        this->signalHolder_);
    getSettings()->showUsercardRoleManagementMenu.connect(
        [this](bool) {
            this->userStateChanged_.invoke();
        },
        this->signalHolder_);
    getSettings()->showUsercardSevenTVPaint.connect(
        [this](bool) {
            this->refreshSeventvPaint();
        },
        this->signalHolder_);
    getSettings()->showSevenTVUsercardButton.connect(
        [this](bool enabled) {
            if (enabled && this->seventvUserID_.isEmpty() &&
                !this->seventvUserLookupInFlight_ &&
                !this->seventvUserLookupFinished_ && !this->userId_.isEmpty())
            {
                auto userID = this->userId_;
                const QStringView kickPrefix = u"kick:";
                if (this->isKick_ && userID.startsWith(kickPrefix))
                {
                    userID = userID.mid(kickPrefix.size());
                }
                this->loadSevenTVAvatar(userID, this->isKick_, false);
                return;
            }

            this->refreshSevenTVUserButtonVisibility();
        },
        this->signalHolder_);
    getSettings()->showUsercardNameHistoryButton.connect(
        [this](bool) {
            this->updateNameHistoryButton();
        },
        this->signalHolder_);
    getSettings()->showFollowButtonInUsercard.connect(
        [this](bool enabled) {
            if (enabled && !this->isKick_ && !this->userId_.isEmpty())
            {
                this->refreshFollowingStatus(false);
            }
            this->updateUsercardFollowButton();
        },
        this->signalHolder_);
    getSettings()->showUsercardLiveViewerCount.connect(
        [this](bool) {
            this->updateLiveIndicatorDisplay();
        },
        this->signalHolder_);
    getSettings()->showUsercardLoadMoreMessagesButton.connect(
        [this](bool) {
            this->updateLoadMoreMessagesButton();
        },
        this->signalHolder_);
    getSettings()->alwaysLoadMoreUsercardMessages.connect(
        [this](bool enabled) {
            if (!enabled)
            {
                this->usercardMessagesLazyLoadEnabled_ = false;
                this->updateLoadMoreMessagesButton();
                return;
            }

            this->maybeStartUsercardMessageAutoLoad();
        },
        this->signalHolder_);
    getSettings()->moltorinoAuthAccounts.connect(
        [this](const QString &, auto) {
            if (!this->isKick_ && !this->userId_.isEmpty() &&
                getSettings()->showFollowButtonInUsercard)
            {
                this->refreshFollowingStatus(true);
            }
            this->updateUsercardFollowButton();
            this->userStateChanged_.invoke();
            this->updateLoadMoreMessagesButton();
            this->maybeStartUsercardMessageAutoLoad();
        },
        this->signalHolder_);
    this->signalHolder_.managedConnect(
        getApp()->getWindows()->gifRepaintRequested, [this] {
            if (!this->isVisible() ||
                !getSettings()->showUsercardSevenTVPaint ||
                !this->seventvPaint_ || !this->seventvPaint_->animated())
            {
                return;
            }

            this->updateSeventvPaintPixmap();
        });
}

void UserInfoPopup::refreshTargetModerationStatus()
{
    if (this->userName_.isEmpty() || !this->underlyingChannel_ ||
        !this->underlyingChannel_->isTwitchChannel())
    {
        return;
    }

    this->isBroadcaster_ =
        this->userName_.compare(this->underlyingChannel_->getName(),
                                Qt::CaseInsensitive) == 0;

    for (const auto &message : this->underlyingChannel_->getMessageSnapshot())
    {
        this->updateTargetModerationStatusFromMessage(message);

        if (this->isMod_ && this->isBroadcaster_)
        {
            break;
        }
    }
}

bool UserInfoPopup::updateTargetModerationStatusFromMessage(
    const MessagePtr &message)
{
    if (message == nullptr || this->userName_.isEmpty())
    {
        return false;
    }

    if (message->loginName.compare(this->userName_, Qt::CaseInsensitive) != 0)
    {
        return false;
    }

    bool changed = false;
    if (!this->isMod_ && (messageHasTwitchBadge(*message, u"moderator") ||
                          messageHasTwitchBadge(*message, u"lead_moderator")))
    {
        this->isMod_ = true;
        changed = true;
    }
    if (!this->isBroadcaster_ &&
        (messageHasTwitchBadge(*message, u"broadcaster") ||
         (this->underlyingChannel_ &&
          this->userName_.compare(this->underlyingChannel_->getName(),
                                  Qt::CaseInsensitive) == 0)))
    {
        this->isBroadcaster_ = true;
        changed = true;
    }

    return changed;
}

bool UserInfoPopup::shouldShowModerationActions() const
{
    if (this->userName_.isEmpty() || !this->underlyingChannel_)
    {
        return false;
    }

    if (auto *twitchChannel =
            dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get()))
    {
        const bool isMyself =
            getApp()->getAccounts()->twitch.getCurrent()->getUserName().compare(
                this->userName_, Qt::CaseInsensitive) == 0;
        if (isMyself || !twitchChannel->hasModRights())
        {
            return false;
        }
        if (twitchChannel->isBroadcaster())
        {
            return true;
        }

        if (!getSettings()->hideModActionsOnModUsercards)
        {
            return true;
        }

        if (!this->isMod_ && !this->isBroadcaster_)
        {
            return true;
        }

        return getSettings()->showModActionsOnModUsercardsAsLeadMod &&
               twitchChannel->isLeadMod() && this->isMod_ &&
               !this->isBroadcaster_;
    }

    if (auto *kickChannel =
            dynamic_cast<KickChannel *>(this->underlyingChannel_.get()))
    {
        const bool isMyself =
            getApp()->getAccounts()->kick.current()->username().compare(
                this->userName_, Qt::CaseInsensitive) == 0;
        return kickChannel->hasModRights() && !isMyself;
    }

    return false;
}

void UserInfoPopup::setData(const QString &name, const ChannelPtr &channel)
{
    this->setData(name, channel, channel);
}

void UserInfoPopup::setData(const QString &name,
                            const ChannelPtr &contextChannel,
                            const ChannelPtr &openingChannel)
{
    const QStringView idPrefix = u"id:";
    bool isId = name.startsWith(idPrefix);
    if (isId)
    {
        this->userId_ = name.mid(idPrefix.size());
        this->updateNotes();
        this->userName_ = "";
    }
    else
    {
        this->userId_.clear();
        this->userName_ = name;
        this->kickUserSlug_ = KickApi::slugify(name);
    }

    this->channel_ = openingChannel;

    if (!contextChannel->isEmpty())
    {
        this->underlyingChannel_ = contextChannel;
    }
    else
    {
        this->underlyingChannel_ = openingChannel;
    }
    this->twitchUserStateConnection_.reset();
    if (auto *twitchChannel =
            dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get()))
    {
        QPointer<UserInfoPopup> self(this);
        this->twitchUserStateConnection_ =
            std::make_unique<pajlada::Signals::ScopedConnection>(
                twitchChannel->userStateChanged.connect([self] {
                    QTimer::singleShot(0, self, [self] {
                        if (!self)
                        {
                            return;
                        }

                        self->userStateChanged_.invoke();
                    });
                }));
    }

    this->setWindowTitle(
        TEXT_TITLE.arg(name, this->underlyingChannel_->getName()));
    this->isKick_ = this->underlyingChannel_->getType() == Channel::Type::Kick;
    if (this->isKick_)
    {
        this->ui_.timeoutWidget->setMinTimeout(60);
    }

    this->resetNameHistory();
    this->resetUsercardMessageLoader();
    this->isMod_ = false;
    this->isBroadcaster_ = false;
    this->userDataRequestGeneration_++;
    this->seventvUserRequestGeneration_++;
    this->seventvUserID_.clear();
    this->seventvUserLookupInFlight_ = false;
    this->seventvUserLookupFinished_ = false;
    this->refreshSevenTVUserButtonVisibility();
    this->refreshSeventvPaint();
    this->refreshTargetModerationStatus();
    if (!this->isKick_)
    {
        if (auto *twitchChannel =
                dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get()))
        {
            twitchChannel->refreshLeadModStatus();
        }
    }

    this->ui_.nameLabel->setText(name);
    this->ui_.nameLabel->setProperty("copy-text", name);
    this->resetUsercardInfoRows();
    this->resetFollowingStatus();
    this->updateUsercardFollowButton();

    if (this->isKick_)
    {
        this->updateKickUserData();
    }
    else
    {
        this->updateUserData();
        if (!this->userId_.isEmpty())
        {
            this->refreshFollowingStatus(false);
        }
    }

    this->userStateChanged_.invoke();

    if (!isId)
    {
        this->updateLatestMessages();
    }
    // If we're opening by ID, this will be called as soon as we get the information from twitch

    auto type = this->channel_->getType();
    if (type == Channel::Type::TwitchLive ||
        type == Channel::Type::TwitchWhispers || type == Channel::Type::Misc ||
        type == Channel::Type::Kick)
    {
        // not a normal twitch channel, the url opened by the button will be invalid, so hide the button
        this->ui_.usercardLabel->hide();
        this->ui_.userlogsLabel->hide();
    }
    else
    {
        this->ui_.usercardLabel->show();
        this->ui_.userlogsLabel->show();
    }
}

void UserInfoPopup::updateLatestMessages()
{
    this->usercardMessagesChannel_ =
        filterMessages(this->userName_, this->underlyingChannel_);
    this->ui_.latestMessages->setChannel(this->usercardMessagesChannel_);
    this->ui_.latestMessages->setSourceChannel(this->underlyingChannel_);

    this->updateUsercardMessagesVisibility();
    this->maybeStartUsercardMessageAutoLoad();

    this->refreshConnection_ =
        std::make_unique<pajlada::Signals::ScopedConnection>(
            this->underlyingChannel_->messageAppended.connect(
                [this](auto message, auto) {
                    if (this->updateTargetModerationStatusFromMessage(message))
                    {
                        this->userStateChanged_.invoke();
                    }

                    if (!checkMessageUserName(this->userName_, message))
                    {
                        return;
                    }

                    if (this->usercardMessagesChannel_ &&
                        this->usercardMessagesChannel_->hasMessages())
                    {
                        this->usercardMessagesChannel_->addMessage(
                            message, MessageContext::Repost);
                        this->updateUsercardMessagesVisibility();
                    }
                    else
                    {
                        // The ChannelView is currently hidden, so manually refresh
                        // and display the latest messages
                        this->updateLatestMessages();
                    }
                }));
}

void UserInfoPopup::updateUsercardMessagesVisibility()
{
    const bool hasMessages = this->usercardMessagesChannel_ &&
                             this->usercardMessagesChannel_->hasMessages();
    const bool hadMessages = this->ui_.latestMessages->isVisible();
    const bool hadNoMessagesLabel = this->ui_.noMessagesLabel->isVisible();
    const bool hadLoadMoreButton = this->ui_.loadMoreMessages != nullptr &&
                                   this->ui_.loadMoreMessages->isVisible();
    const auto previousNoMessagesText = this->ui_.noMessagesLabel->getText();
    const auto noMessagesText = this->usercardMessagesLoading_
                                    ? QStringLiteral("Loading messages...")
                                    : QStringLiteral("No recent messages");

    this->ui_.latestMessages->setVisible(hasMessages);
    this->ui_.noMessagesLabel->setText(noMessagesText);
    this->ui_.noMessagesLabel->setVisible(!hasMessages);
    this->updateLoadMoreMessagesButton();

    const bool hasLoadMoreButton = this->ui_.loadMoreMessages != nullptr &&
                                   this->ui_.loadMoreMessages->isVisible();
    if (hadMessages != hasMessages || hadNoMessagesLabel != !hasMessages ||
        hadLoadMoreButton != hasLoadMoreButton ||
        previousNoMessagesText != noMessagesText)
    {
        this->adjustSize();
    }
}

void UserInfoPopup::resetUsercardMessageLoader()
{
    ++this->usercardMessagesRequestGeneration_;
    this->usercardMessagesCursor_.clear();
    this->usercardMessagesError_.clear();
    this->usercardMessagesLoading_ = false;
    this->usercardMessagesHasNextPage_ = true;
    this->usercardMessagesLazyLoadEnabled_ =
        getSettings()->alwaysLoadMoreUsercardMessages;
    this->usercardMessagesChannel_.reset();
    this->updateLoadMoreMessagesButton();
}

bool UserInfoPopup::canLoadMoreUsercardMessages() const
{
    if (this->isKick_ || this->userName_.isEmpty() || this->userId_.isEmpty() ||
        !this->underlyingChannel_)
    {
        return false;
    }

    auto *twitchChannel =
        dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get());
    if (twitchChannel == nullptr || twitchChannel->roomId().isEmpty())
    {
        return false;
    }

    const auto auth = MoltorinoAuth::resolveModerationToken(
        twitchChannel->roomId(), twitchChannel->getName());
    if (!auth.hasToken())
    {
        return false;
    }

    return !auth.legacy || twitchChannel->hasModRights();
}

void UserInfoPopup::updateLoadMoreMessagesButton()
{
    auto *button = this->ui_.loadMoreMessages;
    if (button == nullptr)
    {
        return;
    }

    const bool canLoad = getSettings()->showUsercardLoadMoreMessagesButton &&
                         this->canLoadMoreUsercardMessages() &&
                         this->usercardMessagesHasNextPage_ &&
                         !this->usercardMessagesLazyLoadEnabled_;
    button->setVisible(canLoad);
    button->setEnabled(canLoad && !this->usercardMessagesLoading_);

    if (this->usercardMessagesLoading_)
    {
        button->setText("Loading messages...");
        button->setToolTip("Loading older messages from Twitch mod logs");
    }
    else
    {
        button->setText("Load more messages");
        button->setToolTip(this->usercardMessagesError_.isEmpty()
                               ? "Load older messages from Twitch mod logs"
                               : "Couldn't load messages. Try again.");
    }
}

void UserInfoPopup::maybeStartUsercardMessageAutoLoad()
{
    if (!getSettings()->alwaysLoadMoreUsercardMessages ||
        !this->canLoadMoreUsercardMessages() ||
        !this->usercardMessagesHasNextPage_)
    {
        return;
    }

    this->usercardMessagesLazyLoadEnabled_ = true;
    this->updateLoadMoreMessagesButton();

    const bool hasMessages = this->usercardMessagesChannel_ &&
                             this->usercardMessagesChannel_->hasMessages();
    if (!hasMessages)
    {
        this->requestMoreUsercardMessages(false);
        return;
    }

    this->maybeLoadMoreUsercardMessagesFromScroll();
}

void UserInfoPopup::requestMoreUsercardMessages(bool enableLazyLoadOnSuccess)
{
    if (this->usercardMessagesLoading_ ||
        !this->canLoadMoreUsercardMessages() ||
        !this->usercardMessagesHasNextPage_)
    {
        return;
    }

    this->usercardMessagesError_.clear();
    this->usercardMessagesLoading_ = true;
    this->updateUsercardMessagesVisibility();
    this->fetchMoreUsercardMessages(2, enableLazyLoadOnSuccess);
}

void UserInfoPopup::maybeLoadMoreUsercardMessagesFromScroll()
{
    if (!this->usercardMessagesLazyLoadEnabled_ ||
        this->usercardMessagesLoading_ || !this->usercardMessagesHasNextPage_)
    {
        return;
    }

    auto &scrollbar = this->ui_.latestMessages->getScrollBar();
    if (scrollbar.getDesiredValue() >
        scrollbar.getMinimum() + usercardMessagePreloadDistance(scrollbar))
    {
        return;
    }

    this->requestMoreUsercardMessages(false);
}

void UserInfoPopup::fetchMoreUsercardMessages(int emptyPageSkipsLeft,
                                              bool enableLazyLoadOnSuccess)
{
    auto *twitchChannel =
        dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get());
    if (twitchChannel == nullptr)
    {
        this->usercardMessagesLazyLoadEnabled_ = false;
        this->usercardMessagesLoading_ = false;
        this->updateUsercardMessagesVisibility();
        return;
    }

    QString authError;
    const auto auth = MoltorinoAuth::resolveModerationToken(
        twitchChannel->roomId(), twitchChannel->getName(), &authError);
    if (!auth.hasToken() || (auth.legacy && !twitchChannel->hasModRights()))
    {
        this->usercardMessagesError_ =
            authError.isEmpty()
                ? QStringLiteral("No saved Leafyrino moderator login found.")
                : authError;
        this->usercardMessagesLazyLoadEnabled_ = false;
        this->usercardMessagesLoading_ = false;
        this->updateUsercardMessagesVisibility();
        return;
    }

    const auto generation = this->usercardMessagesRequestGeneration_;
    const auto channelId = twitchChannel->roomId();
    const auto channelName = twitchChannel->getName();
    const auto targetUserId = this->userId_;
    const auto cursor = this->usercardMessagesCursor_;
    const auto oldestLoadedMessage =
        oldestUsercardMessageTime(this->usercardMessagesChannel_);
    const QPointer<UserInfoPopup> self(this);

    TwitchGql::getUsercardMessagesBySender(
        channelId, targetUserId, cursor, auth.token,
        [self, generation, targetUserId, channelName, emptyPageSkipsLeft,
         oldestLoadedMessage,
         enableLazyLoadOnSuccess](GqlUsercardMessagePage page) mutable {
            if (!self ||
                generation != self->usercardMessagesRequestGeneration_ ||
                self->userId_ != targetUserId)
            {
                return;
            }

            self->usercardMessagesCursor_ = page.nextCursor;
            self->usercardMessagesHasNextPage_ =
                page.hasNextPage && !page.nextCursor.isEmpty();

            if (!self->usercardMessagesChannel_)
            {
                self->usercardMessagesChannel_ =
                    std::make_shared<TwitchChannel>(channelName);
                self->ui_.latestMessages->setChannel(
                    self->usercardMessagesChannel_);
                self->ui_.latestMessages->setSourceChannel(
                    self->underlyingChannel_);
            }

            std::vector<MessagePtr> messages;
            messages.reserve(static_cast<size_t>(page.messages.size()));
            auto *renderChannel =
                dynamic_cast<TwitchChannel *>(self->underlyingChannel_.get());
            for (auto it = page.messages.crbegin(); it != page.messages.crend();
                 ++it)
            {
                const auto sentAt = parseIvrTimestamp(it->sentAt);
                if (oldestLoadedMessage.isValid() && sentAt.isValid() &&
                    sentAt >= oldestLoadedMessage)
                {
                    continue;
                }

                if (self->usercardMessagesChannel_->findMessageByID(it->id))
                {
                    continue;
                }

                messages.push_back(makeUsercardModLogMessage(
                    *it, renderChannel, channelName, targetUserId));
            }

            if (!messages.empty())
            {
                self->usercardMessagesChannel_->addMessagesAtStart(messages);
                if (enableLazyLoadOnSuccess)
                {
                    self->usercardMessagesLazyLoadEnabled_ = true;
                }
                self->usercardMessagesLoading_ = false;
                self->usercardMessagesError_.clear();
                self->updateUsercardMessagesVisibility();
                QTimer::singleShot(0, self.data(), [self] {
                    if (self)
                    {
                        self->maybeLoadMoreUsercardMessagesFromScroll();
                    }
                });
                return;
            }

            if (self->usercardMessagesHasNextPage_ && emptyPageSkipsLeft > 0)
            {
                self->fetchMoreUsercardMessages(emptyPageSkipsLeft - 1,
                                                enableLazyLoadOnSuccess);
                return;
            }

            self->usercardMessagesLoading_ = false;
            self->updateUsercardMessagesVisibility();
        },
        [self, generation](const QString &error) {
            if (!self || generation != self->usercardMessagesRequestGeneration_)
            {
                return;
            }

            qCWarning(chatterinoWidget)
                << "Failed to load usercard messages:" << error;
            self->usercardMessagesError_ = error;
            self->usercardMessagesLazyLoadEnabled_ = false;
            self->usercardMessagesLoading_ = false;
            self->updateUsercardMessagesVisibility();
        });
}

void UserInfoPopup::updateUserData()
{
    this->ui_.userlogsLabel->setVisible(true);

    std::weak_ptr<bool> hack = this->lifetimeHack_;
    const auto requestGeneration = ++this->userDataRequestGeneration_;
    const auto isCurrentRequest = [this, hack, requestGeneration] {
        return hack.lock() &&
               requestGeneration == this->userDataRequestGeneration_;
    };
    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();

    const auto onUserFetchFailed = [this, isCurrentRequest] {
        if (!isCurrentRequest())
        {
            return;
        }

        // this can occur when the account doesn't exist.
        if (getSettings()->showUsercardFollowerCount)
        {
            this->ui_.followerCountLabel->setText(
                TEXT_FOLLOWERS.arg(TEXT_UNAVAILABLE));
            this->ui_.followerCountLabel->setVisible(true);
        }
        if (getSettings()->showUsercardCreatedDate)
        {
            this->ui_.createdDateLabel->setText(
                TEXT_CREATED.arg(TEXT_UNAVAILABLE));
            this->ui_.createdDateLabel->setVisible(true);
        }

        this->ui_.nameLabel->setText(this->userName_);

        this->ui_.userIDLabel->setText(u"ID " % TEXT_UNAVAILABLE);
        this->ui_.userIDLabel->setProperty("copy-text",
                                           TEXT_UNAVAILABLE.toString());

        if (getSettings()->showUsercardFollowage)
        {
            this->ui_.followageLabel->setText({});
        }
        if (getSettings()->showUsercardSubage)
        {
            this->ui_.subageLabel->setText({});
        }
        this->hideUsercardSubGiftRow();
        if (getSettings()->showUsercardChatterCount)
        {
            this->ui_.chatterCountLabel->setText("Chatters: " %
                                                 TEXT_UNAVAILABLE);
        }
        if (getSettings()->showUsercardLastLive)
        {
            this->ui_.lastLiveLabel->setText("Last live: " % TEXT_UNAVAILABLE);
        }
        if (getSettings()->showUsercardColor)
        {
            this->ui_.userColorRow->setProperty("copy-color", {});
            this->ui_.userColorRow->setProperty("swatch-color", {});
            this->ui_.userColorLabel->setText("Color: " % TEXT_UNAVAILABLE);
            this->updateUsercardStatusIcons();
        }
        if (getSettings()->showUsercardStatus)
        {
            this->ui_.statusLabel->setText("Status: " % TEXT_UNAVAILABLE);
        }

        this->seventvUserRequestGeneration_++;
        this->seventvUserID_.clear();
        this->seventvUserLookupInFlight_ = false;
        this->seventvUserLookupFinished_ = true;
        this->refreshSevenTVUserButtonVisibility();
    };
    const auto onUserFetched = [this, isCurrentRequest,
                                currentUser](const HelixUser &user) {
        if (!isCurrentRequest())
        {
            return;
        }

        this->userId_ = user.id;
        this->refreshFollowingStatus(false);
        this->updateUsercardFollowButton();

        // Correct for when being opened with ID
        if (this->userName_.isEmpty())
        {
            this->userName_ = user.login;
            this->ui_.nameLabel->setText(user.login);

            this->refreshTargetModerationStatus();
            this->userStateChanged_.invoke();

            // Ensure recent messages are shown
            this->updateLatestMessages();
        }

        this->resetNameHistory();
        this->updateLoadMoreMessagesButton();
        this->maybeStartUsercardMessageAutoLoad();
        this->helixAvatarUrl_ = user.profileImageUrl;
        this->updateAvatarUrl();
        this->updateNotes();

        // copyable button for login name of users with a localized username
        if (user.displayName.toLower() != user.login)
        {
            this->ui_.localizedNameLabel->setText(user.displayName);
            this->ui_.localizedNameLabel->setProperty("copy-text",
                                                      user.displayName);
            this->ui_.localizedNameLabel->setVisible(true);
            this->ui_.localizedNameCopyButton->setVisible(true);
        }
        else
        {
            this->ui_.nameLabel->setText(user.displayName);
            this->ui_.nameLabel->setProperty("copy-text", user.displayName);
        }

        this->setWindowTitle(TEXT_TITLE.arg(
            user.displayName, this->underlyingChannel_->getName()));
        if (getSettings()->showUsercardCreatedDate)
        {
            this->ui_.createdDateLabel->setText(
                TEXT_CREATED.arg(user.createdAt.section("T", 0, 0)));
            this->ui_.createdDateLabel->setToolTip(
                formatLongFriendlyDuration(
                    QDateTime::fromString(user.createdAt, Qt::ISODateWithMs),
                    QDateTime::currentDateTimeUtc()) +
                u" ago"_s);
            this->ui_.createdDateLabel->setMouseTracking(true);
            this->ui_.createdDateLabel->setVisible(true);
        }
        this->ui_.userIDLabel->setText(TEXT_USER_ID % user.id);
        this->ui_.userIDLabel->setProperty("copy-text", user.id);

        if (getApp()->getStreamerMode()->isEnabled() &&
            getSettings()->streamerModeHideUsercardAvatars)
        {
            this->ui_.avatarButton->setPixmap(getResources().streamerMode);
            if (getSettings()->showSevenTVUsercardButton)
            {
                this->loadSevenTVAvatar(user.id, false, false);
            }
        }
        else
        {
            this->loadAvatar(user.id, user.profileImageUrl, false);
        }

        if (getSettings()->showUsercardFollowerCount)
        {
            getHelix()->getChannelFollowers(
                user.id,
                [this, isCurrentRequest](const auto &followers) {
                    if (!isCurrentRequest() ||
                        !getSettings()->showUsercardFollowerCount)
                    {
                        return;
                    }
                    this->ui_.followerCountLabel->setText(
                        TEXT_FOLLOWERS.arg(localizeNumbers(followers.total)));
                    this->ui_.followerCountLabel->setVisible(true);
                },
                [](const auto &errorMessage) {
                    qCWarning(chatterinoTwitch)
                        << "Error getting followers:" << errorMessage;
                });
        }
        getHelix()->getStreamById(
            user.id,
            [this, isCurrentRequest](bool isLive, const auto &stream) {
                if (!isCurrentRequest())
                {
                    return;
                }

                if (isLive)
                {
                    this->isUserLive_ = true;
                    this->liveViewerCount_ = stream.viewerCount;
                }
                else
                {
                    this->isUserLive_ = false;
                    this->liveViewerCount_ = 0;
                }
                this->updateLiveIndicatorDisplay();
            },
            [id{user.id}]() {
                qCWarning(chatterinoWidget)
                    << "Failed to get stream for user ID" << id;
            },
            []() {});

        // get ignore state
        bool isIgnoring = currentUser->blockedUserIds().contains(user.id);

        // get ignoreHighlights state
        bool isIgnoringHighlights = false;
        const auto &vector = getSettings()->blacklistedUsers.raw();
        for (const auto &blockedUser : vector)
        {
            if (this->userName_ == blockedUser.getPattern())
            {
                isIgnoringHighlights = true;
                break;
            }
        }
        if (getSettings()->isBlacklistedUser(this->userName_) &&
            !isIgnoringHighlights)
        {
            this->ui_.ignoreHighlights->setToolTip("Name matched by regex");
        }
        else
        {
            this->ui_.ignoreHighlights->setEnabled(true);
        }
        this->ui_.block->setChecked(isIgnoring);
        this->ui_.block->setEnabled(true);
        this->ui_.ignoreHighlights->setChecked(isIgnoringHighlights);
        this->ui_.notesAdd->setEnabled(true);

        auto type = this->underlyingChannel_->getType();

        if (type == Channel::Type::Twitch)
        {
            // get followage and subage
            if (getSettings()->showUsercardFollowage ||
                getSettings()->showUsercardSubage ||
                getSettings()->showUsercardSubGiftGifter)
            {
                getIvr()->getSubage(
                    this->userName_, this->underlyingChannel_->getName(),
                    [this, isCurrentRequest](const IvrSubage &subageInfo) {
                        if (!isCurrentRequest())
                        {
                            return;
                        }

                        if (getSettings()->showUsercardFollowage &&
                            !subageInfo.followingSince.isEmpty())
                        {
                            const auto followedAt =
                                parseIvrTimestamp(subageInfo.followingSince);

                            if (followedAt.isValid())
                            {
                                if (this->isFollowing() &&
                                    (!this->followedAt_ ||
                                     !this->followedAt_->isValid()))
                                {
                                    this->followedAt_ = followedAt;
                                    this->updateUsercardFollowButton();
                                }

                                const auto followedDate = followedAt.date();
                                const auto followingSince =
                                    followedDate.toString(Qt::ISODate);
                                auto relativeTime = QString();
                                if (getSettings()
                                        ->showUsercardFollowageRelativeTime)
                                {
                                    relativeTime =
                                        formatUsercardFollowRelativeTime(
                                            followedDate);
                                }
                                this->ui_.followageLabel->setText(
                                    "Following since " + followingSince +
                                    relativeTime);
                                this->ui_.followageLabel->setToolTip(
                                    formatLongFriendlyDuration(
                                        followedAt,
                                        QDateTime::currentDateTimeUtc()) +
                                    u" ago"_s);
                                this->ui_.followageLabel->setMouseTracking(
                                    true);
                                this->updateUsercardStatusIcons();
                                this->ui_.followageRow->setVisible(true);
                                this->ui_.followageIcon->setVisible(true);
                            }
                            else
                            {
                                this->ui_.followageLabel->setText({});
                                this->ui_.followageRow->setVisible(true);
                                this->ui_.followageIcon->setVisible(false);
                            }
                        }
                        else if (getSettings()->showUsercardFollowage)
                        {
                            this->ui_.followageLabel->setText({});
                            this->ui_.followageRow->setVisible(true);
                            this->ui_.followageIcon->setVisible(false);
                        }

                        if (!getSettings()->showUsercardSubage)
                        {
                            this->ui_.subageLabel->setText({});
                            this->ui_.subageRow->setVisible(false);
                            this->ui_.subageIcon->setVisible(false);
                            this->updateUsercardSubGiftRow(subageInfo);
                            return;
                        }

                        if (subageInfo.isSubHidden)
                        {
                            this->ui_.subageLabel->setText(
                                "Subscription status hidden");
                            this->updateUsercardStatusIcons();
                            this->ui_.subageRow->setVisible(true);
                            this->ui_.subageIcon->setVisible(false);
                        }
                        else if (subageInfo.isSubbed)
                        {
                            auto subageText =
                                QString("Tier %1 - Subscribed for %2 months")
                                    .arg(subageInfo.subTier)
                                    .arg(subageInfo.totalSubMonths);
                            if (getSettings()->showUsercardSubageRelativeTime)
                            {
                                subageText += formatUsercardYearsMonths(
                                    subageInfo.totalSubMonths);
                            }
                            this->ui_.subageLabel->setText(subageText);
                            this->updateUsercardStatusIcons();
                            this->ui_.subageRow->setVisible(true);
                            this->ui_.subageIcon->setVisible(true);
                        }
                        else if (subageInfo.totalSubMonths)
                        {
                            auto subageText =
                                QString("Previously subscribed for %1 months")
                                    .arg(subageInfo.totalSubMonths);
                            if (getSettings()->showUsercardSubageRelativeTime)
                            {
                                subageText += formatUsercardYearsMonths(
                                    subageInfo.totalSubMonths);
                            }
                            this->ui_.subageLabel->setText(subageText);
                            this->updateUsercardStatusIcons();
                            this->ui_.subageRow->setVisible(true);
                            this->ui_.subageIcon->setVisible(true);
                        }
                        else
                        {
                            this->ui_.subageLabel->setText({});
                            this->ui_.subageRow->setVisible(true);
                            this->ui_.subageIcon->setVisible(false);
                        }

                        this->updateUsercardSubGiftRow(subageInfo);
                    },
                    [this, isCurrentRequest] {
                        if (!isCurrentRequest())
                        {
                            return;
                        }

                        if (getSettings()->showUsercardFollowage)
                        {
                            this->ui_.followageLabel->setText({});
                            this->ui_.followageRow->setVisible(true);
                            this->ui_.followageIcon->setVisible(false);
                        }
                        if (getSettings()->showUsercardSubage)
                        {
                            this->ui_.subageLabel->setText({});
                            this->ui_.subageRow->setVisible(true);
                            this->ui_.subageIcon->setVisible(false);
                        }
                        if (getSettings()->showUsercardSubGiftGifter)
                        {
                            this->hideUsercardSubGiftRow();
                        }
                    });
            }

            getIvr()->getUser(
                user.login,
                [this, isCurrentRequest](const IvrUserProfile &profile) {
                    if (!isCurrentRequest())
                    {
                        return;
                    }

                    this->applyIvrUserProfile(profile);
                },
                [this, isCurrentRequest] {
                    if (!isCurrentRequest())
                    {
                        return;
                    }

                    if (getSettings()->showUsercardChatterCount)
                    {
                        this->ui_.chatterCountLabel->setText("Chatters: " %
                                                             TEXT_UNAVAILABLE);
                    }
                    if (getSettings()->showUsercardLastLive)
                    {
                        this->ui_.lastLiveLabel->setText("Last live: " %
                                                         TEXT_UNAVAILABLE);
                    }
                    if (getSettings()->showUsercardColor)
                    {
                        this->ui_.userColorRow->setProperty("copy-color", {});
                        this->ui_.userColorRow->setProperty("swatch-color", {});
                        this->ui_.userColorLabel->setText("Color: " %
                                                          TEXT_UNAVAILABLE);
                        this->updateUsercardStatusIcons();
                    }
                    if (getSettings()->showUsercardStatus)
                    {
                        this->ui_.statusLabel->setText("Status: " %
                                                       TEXT_UNAVAILABLE);
                    }
                });
        }

        // get roles
        getIvr()->getUserRoles(
            this->userName_,
            [this, isCurrentRequest](const IvrResolve &userInfo) {
                if (!isCurrentRequest())
                {
                    return;
                }

                QString rolesString = "";

                if (userInfo.isBot)
                {
                    rolesString += "Bot ";
                }
                if (userInfo.isPartner)
                {
                    rolesString += "Partner ";
                }
                if (userInfo.isAffiliate)
                {
                    rolesString += "Affiliate ";
                }
                if (userInfo.isStaff)
                {
                    rolesString += "Staff ";
                }
                if (userInfo.isExStaff)
                {
                    rolesString += "Ex-Staff ";
                }

                this->ui_.rolesLabel->setText((rolesString));

                if (userInfo.chatterCount >= 0)
                {
                    this->ui_.chatterCountLabel->setText(TEXT_CHATTERS.arg(
                        localizeNumbers(userInfo.chatterCount)));
                }
                else
                {
                    this->ui_.chatterCountLabel->setText(
                        TEXT_CHATTERS.arg(TEXT_UNAVAILABLE));
                }

                if (!userInfo.lastBroadcastStartedAt.isEmpty())
                {
                    this->ui_.lastLiveLabel->setText(TEXT_LAST_LIVE.arg(
                        userInfo.lastBroadcastStartedAt.section("T", 0, 0)));
                    this->ui_.lastLiveLabel->setToolTip(
                        formatLongFriendlyDuration(
                            QDateTime::fromString(
                                userInfo.lastBroadcastStartedAt,
                                Qt::ISODateWithMs),
                            QDateTime::currentDateTimeUtc()) +
                        u" ago"_s);
                    this->ui_.lastLiveLabel->setMouseTracking(true);
                }
                else
                {
                    this->ui_.lastLiveLabel->setText(
                        TEXT_LAST_LIVE.arg(TEXT_UNAVAILABLE));
                }

                if (!userInfo.chatColor.isEmpty())
                {
                    this->ui_.userColorRow->setProperty("copy-color",
                                                        userInfo.chatColor);
                    this->ui_.userColorRow->setProperty("swatch-color",
                                                        userInfo.chatColor);
                    this->ui_.userColorLabel->setText(
                        TEXT_COLOR.arg(userInfo.chatColor));
                    this->updateUsercardStatusIcons();
                }
                else
                {
                    this->ui_.userColorRow->setProperty("copy-color", {});
                    this->ui_.userColorRow->setProperty("swatch-color", {});
                    this->ui_.userColorLabel->setText(
                        TEXT_COLOR.arg(TEXT_UNAVAILABLE));
                    this->updateUsercardStatusIcons();
                }
            },
            [] {});

        // get pronouns
        if (getSettings()->showPronouns)
        {
            getApp()->getPronouns()->getUserPronoun(
                user.login,
                [this, isCurrentRequest](const auto userPronoun) {
                    runInGuiThread([this, isCurrentRequest,
                                    userPronoun = std::move(userPronoun)]() {
                        if (!isCurrentRequest() ||
                            this->ui_.pronounsLabel == nullptr)
                        {
                            return;
                        }
                        if (!userPronoun.isUnspecified())
                        {
                            this->ui_.pronounsLabel->setText(
                                TEXT_PRONOUNS.arg(userPronoun.format()));
                        }
                        else
                        {
                            this->ui_.pronounsLabel->setText(
                                TEXT_PRONOUNS.arg(TEXT_UNSPECIFIED));
                        }
                    });
                },
                [this, isCurrentRequest]() {
                    runInGuiThread([this, isCurrentRequest]() {
                        qCWarning(chatterinoTwitch) << "Error getting pronouns";
                        if (!isCurrentRequest())
                        {
                            return;
                        }
                        this->ui_.pronounsLabel->setText(
                            TEXT_PRONOUNS.arg(TEXT_UNSPECIFIED));
                    });
                });
        }
    };

    if (!this->userId_.isEmpty())
    {
        getHelix()->getUserById(this->userId_, onUserFetched,
                                onUserFetchFailed);
    }
    else
    {
        getHelix()->getUserByName(this->userName_, onUserFetched,
                                  onUserFetchFailed);
    }

    this->ui_.block->setEnabled(false);
    this->ui_.ignoreHighlights->setEnabled(false);
    this->ui_.notesAdd->setEnabled(false);

    bool isMyself =
        getApp()->getAccounts()->twitch.getCurrent()->getUserName().compare(
            this->userName_, Qt::CaseInsensitive) == 0;
    this->ui_.block->setVisible(!isMyself);
    this->ui_.ignoreHighlights->setVisible(!isMyself);
}

void UserInfoPopup::loadAvatar(const QString &userID, const QString &pictureURL,
                               bool isKick)
{
    auto filename =
        getApp()->getPaths().cacheDirectory() + "/" + hashUrl(pictureURL);
    QFile cacheFile(filename);
    if (cacheFile.exists() && cacheFile.open(QIODevice::ReadOnly))
    {
        QPixmap avatar{};

        avatar.loadFromData(cacheFile.readAll());
        this->ui_.avatarButton->setPixmap(avatar);
        this->avatarPixmap_ = std::move(avatar);
    }
    else
    {
        QNetworkRequest req(pictureURL);
        req.setHeader(QNetworkRequest::UserAgentHeader, "Chatterino");
        static auto *manager = new QNetworkAccessManager();
        auto *reply = manager->get(req);
        QObject::connect(reply, &QNetworkReply::finished, reply,
                         &QObject::deleteLater);

        QObject::connect(reply, &QNetworkReply::finished, this,
                         [this, reply, filename] {
                             if (reply->error() == QNetworkReply::NoError)
                             {
                                 const auto data = reply->readAll();

                                 QPixmap avatar;
                                 avatar.loadFromData(data);
                                 this->ui_.avatarButton->setPixmap(avatar);
                                 this->saveCacheAvatar(data, filename);
                                 this->avatarPixmap_ = std::move(avatar);
                             }
                             else
                             {
                                 this->ui_.avatarButton->setPixmap(QPixmap());
                             }
                         });
    }

    this->helixAvatarUrl_ = pictureURL;
    this->updateAvatarUrl();

    if (getSettings()->displaySevenTVAnimatedProfile ||
        getSettings()->showSevenTVUsercardButton)
    {
        this->loadSevenTVAvatar(userID, isKick);
    }
}

void UserInfoPopup::loadSevenTVAvatar(const QString &userID, bool isKick,
                                      bool allowAvatarDownload)
{
    const auto generation = ++this->seventvUserRequestGeneration_;
    if (userID.isEmpty())
    {
        this->seventvUserID_.clear();
        this->seventvUserLookupInFlight_ = false;
        this->seventvUserLookupFinished_ = true;
        this->refreshSevenTVUserButtonVisibility();
        return;
    }

    auto fmt = isKick ? SEVENTV_KICK_USER_API : SEVENTV_TWITCH_USER_API;
    const auto cacheKey = sevenTVUserCacheKey(userID, isKick);
    const auto cachedIt = sevenTVUserIDCache().constFind(cacheKey);
    const bool hadCachedUserID =
        cachedIt != sevenTVUserIDCache().constEnd() && !cachedIt->isEmpty();
    const bool needsAvatar =
        allowAvatarDownload && getSettings()->displaySevenTVAnimatedProfile;

    if (cachedIt != sevenTVUserIDCache().constEnd())
    {
        this->seventvUserID_ = *cachedIt;
        this->seventvUserLookupInFlight_ = false;
        this->seventvUserLookupFinished_ = true;
        this->refreshSevenTVUserButtonVisibility();

        if (!needsAvatar || this->seventvUserID_.isEmpty())
        {
            return;
        }
    }
    else
    {
        this->seventvUserID_.clear();
        this->seventvUserLookupInFlight_ = true;
        this->seventvUserLookupFinished_ = false;
        this->refreshSevenTVUserButtonVisibility();
    }

    NetworkRequest(fmt.arg(userID))
        .timeout(20000)
        .onSuccess([this, hack = std::weak_ptr<bool>(this->lifetimeHack_),
                    generation, cacheKey,
                    allowAvatarDownload](const NetworkResult &result) {
            if (!hack.lock() ||
                generation != this->seventvUserRequestGeneration_)
            {
                return;
            }

            const auto root = result.parseJson();
            const auto userObj = root["user"].toObject();
            this->seventvUserID_ = userObj["id"].toString();
            if (!this->seventvUserID_.isEmpty())
            {
                sevenTVUserIDCache().insert(cacheKey, this->seventvUserID_);
            }
            this->seventvUserLookupInFlight_ = false;
            this->seventvUserLookupFinished_ = true;
            this->refreshSevenTVUserButtonVisibility();

            if (!allowAvatarDownload ||
                !getSettings()->displaySevenTVAnimatedProfile)
            {
                return;
            }

            auto url = userObj["avatar_url"].toString();

            if (url.isEmpty())
            {
                return;
            }
            if (!url.startsWith(u"https:"))
            {
                url.prepend(u"https:");
            }
            this->seventvAvatarUrl_ = url;
            if (this->helixAvatarUrl_ == this->seventvAvatarUrl_)
            {
                return;
            }

            auto dotIdx = url.lastIndexOf('.') + 1;
            QByteArray format;
            if (dotIdx > 0)
            {
                auto end = url.size();
                auto queryIdx = url.lastIndexOf('?');
                if (queryIdx > dotIdx)
                {
                    end = queryIdx;
                }
                format = QStringView(url).sliced(dotIdx, end - dotIdx).toUtf8();
            }

            // We're implementing custom caching here,
            // because we need the cached file path.
            auto hash = hashUrl(url);
            auto filename = getApp()->getPaths().cacheDirectory() + "/" + hash;

            QFile cacheFile(filename);
            if (cacheFile.exists())
            {
                this->setSevenTVAvatar(filename, format);
                return;
            }

            QNetworkRequest req(url);

            // We're using this manager instead of the one provided
            // in NetworkManager, because we're on a different thread.
            static auto *manager = new QNetworkAccessManager();
            auto *reply = manager->get(req);
            QObject::connect(reply, &QNetworkReply::finished, reply,
                             &QObject::deleteLater);

            QObject::connect(reply, &QNetworkReply::finished, this,
                             [this, reply, url, filename, format] {
                                 if (reply->error() == QNetworkReply::NoError)
                                 {
                                     this->saveCacheAvatar(reply->readAll(),
                                                           filename);
                                     this->setSevenTVAvatar(filename, format);
                                 }
                                 else
                                 {
                                     qCWarning(chatterinoSeventv)
                                         << "Error fetching Profile Picture:"
                                         << reply->error();
                                 }
                             });

            return;
        })
        .onError([this, hack = std::weak_ptr<bool>(this->lifetimeHack_),
                  generation, cacheKey,
                  hadCachedUserID](const NetworkResult &result) {
            if (!hack.lock() ||
                generation != this->seventvUserRequestGeneration_)
            {
                return;
            }

            const auto status = result.status();
            if (status && *status == 404)
            {
                sevenTVUserIDCache().insert(cacheKey, {});
            }
            else if (hadCachedUserID)
            {
                return;
            }

            this->seventvUserID_.clear();
            this->seventvUserLookupInFlight_ = false;
            this->seventvUserLookupFinished_ = true;
            this->refreshSevenTVUserButtonVisibility();
        })
        .execute();
}

void UserInfoPopup::setSevenTVAvatar(const QString &filename,
                                     const QByteArray &format)
{
    auto *movie = new QMovie(filename, format, this);
    if (!movie->isValid())
    {
        qCWarning(chatterinoSeventv)
            << "Error reading Profile Picture, " << movie->lastErrorString();
        return;
    }

    QObject::connect(movie, &QMovie::frameChanged, this, [this, movie] {
        this->ui_.avatarButton->setPixmap(movie->currentPixmap());
    });

    movie->start();
    this->seventvAvatar_ = movie;
    this->ui_.switchAvatars->show();
    this->ui_.switchAvatars->setText(u"Show " % this->platformName());
    this->isTwitchAvatarShown_ = false;
    this->updateAvatarUrl();
}

void UserInfoPopup::saveCacheAvatar(const QByteArray &avatar,
                                    const QString &filename) const
{
    QFile outfile(filename);
    if (outfile.open(QIODevice::WriteOnly))
    {
        if (outfile.write(avatar) == -1)
        {
            qCWarning(chatterinoImage) << "Error writing to cache" << filename;
            this->ui_.avatarButton->setPixmap(QPixmap());
        }
    }
    else
    {
        qCWarning(chatterinoImage) << "Error writing to cache" << filename;
        this->ui_.avatarButton->setPixmap(QPixmap());
    }
}

void UserInfoPopup::updateNotes()
{
    static QRegularExpression onlySpaceRegex{"^\\s*$"};

    auto userData = getApp()->getUserData()->getUser(this->userId_);
    if (!userData.has_value() ||
        onlySpaceRegex.match(userData->notes).hasMatch())
    {
        this->ui_.notesPreview->setText("");
        this->ui_.notesPreview->setVisible(false);
        return;
    }

    this->ui_.notesPreview->setText(userData->notes);
    this->ui_.notesPreview->setVisible(true);
}

void UserInfoPopup::updateKickUserData()
{
    assert(this->isKick_);

    auto onChannelFetchFailed = [](UserInfoPopup *self) {
        // this can occur when the account doesn't exist.
        if (getSettings()->showUsercardFollowerCount)
        {
            self->ui_.followerCountLabel->setText(
                TEXT_FOLLOWERS.arg(TEXT_UNAVAILABLE));
            self->ui_.followerCountLabel->setVisible(true);
        }
        if (getSettings()->showUsercardCreatedDate)
        {
            self->ui_.createdDateLabel->setText(
                TEXT_CREATED.arg(TEXT_UNAVAILABLE));
            self->ui_.createdDateLabel->setVisible(true);
        }

        self->ui_.nameLabel->setText(self->userName_);

        self->ui_.userIDLabel->setText(u"ID " % TEXT_UNAVAILABLE);
        self->ui_.userIDLabel->setProperty("copy-text",
                                           TEXT_UNAVAILABLE.toString());

        self->seventvUserRequestGeneration_++;
        self->seventvUserID_.clear();
        self->seventvUserLookupInFlight_ = false;
        self->seventvUserLookupFinished_ = true;
        self->refreshSevenTVUserButtonVisibility();
    };
    auto onChannelFetched = [](UserInfoPopup *self,
                               const KickPrivateChannelInfo &channel) {
        // Correct for when being opened with ID
        if (self->userName_.isEmpty())
        {
            self->userName_ = channel.user.username;
            self->kickUserSlug_ = channel.slug;
            self->ui_.nameLabel->setText(channel.user.username);

            // Ensure recent messages are shown
            self->updateLatestMessages();
        }

        self->kickUserID_ = channel.user.userID;
        auto userIDStr = QString::number(self->kickUserID_);
        self->userId_ = u"kick:" % userIDStr;
        self->helixAvatarUrl_ = channel.user.profilePictureURL.value_or(
            u"https://kick.com/img/default-profile-pictures/default-avatar-2.webp"_s);
        self->updateAvatarUrl();
        self->updateNotes();

        self->ui_.nameLabel->setText(channel.user.username);
        self->ui_.nameLabel->setProperty("copy-text", channel.user.username);

        self->setWindowTitle(TEXT_TITLE.arg(
            channel.user.username, self->underlyingChannel_->getName()));
        if (getSettings()->showUsercardCreatedDate)
        {
            self->ui_.createdDateLabel->setText(TEXT_CREATED.arg(
                channel.chatroom.createdAt.date().toString(Qt::ISODate)));
            self->ui_.createdDateLabel->setToolTip(
                formatLongFriendlyDuration(channel.chatroom.createdAt,
                                           QDateTime::currentDateTimeUtc()) +
                u" ago"_s);
            self->ui_.createdDateLabel->setMouseTracking(true);
            self->ui_.createdDateLabel->setVisible(true);
        }
        self->ui_.userIDLabel->setText(TEXT_USER_ID % userIDStr);
        self->ui_.userIDLabel->setProperty("copy-text", userIDStr);

        if (getApp()->getStreamerMode()->isEnabled() &&
            getSettings()->streamerModeHideUsercardAvatars)
        {
            self->ui_.avatarButton->setPixmap(getResources().streamerMode);
            if (getSettings()->showSevenTVUsercardButton)
            {
                self->loadSevenTVAvatar(userIDStr, true, false);
            }
        }
        else
        {
            self->loadAvatar(userIDStr, self->helixAvatarUrl_, true);
        }

        if (getSettings()->showUsercardFollowerCount)
        {
            self->ui_.followerCountLabel->setText(
                TEXT_FOLLOWERS.arg(localizeNumbers(channel.followersCount)));
            self->ui_.followerCountLabel->setVisible(true);
        }

        // get ignoreHighlights state
        bool isIgnoringHighlights = false;
        const auto &vector = getSettings()->blacklistedUsers.raw();
        for (const auto &blockedUser : vector)
        {
            if (self->userName_ == blockedUser.getPattern())
            {
                isIgnoringHighlights = true;
                break;
            }
        }
        if (getSettings()->isBlacklistedUser(self->userName_) &&
            !isIgnoringHighlights)
        {
            self->ui_.ignoreHighlights->setToolTip("Name matched by regex");
        }
        else
        {
            self->ui_.ignoreHighlights->setEnabled(true);
        }
        self->ui_.block->setChecked(/*is_ignoring=*/false);
        self->ui_.block->setEnabled(true);
        self->ui_.ignoreHighlights->setChecked(isIgnoringHighlights);
        self->ui_.notesAdd->setEnabled(true);
    };

    // FIXME: this doesn't support opening by user ID

    KickApi::privateChannelInfo(
        this->userName_, [self = QPointer(this), onChannelFetched,
                          onChannelFetchFailed](const auto &res) {
            if (!self)
            {
                return;
            }
            if (res)
            {
                onChannelFetched(self.get(), *res);
            }
            else
            {
                qCDebug(chatterinoKick)
                    << "Channel fetch failed" << res.error();
                onChannelFetchFailed(self.get());
            }
        });
    KickApi::privateUserInChannelInfo(
        this->userName_, this->underlyingChannel_->getName(),
        [self = QPointer(this)](const auto &res) {
            if (!self || !res)
            {
                return;
            }

            if (getSettings()->showUsercardFollowage && res->followingSince)
            {
                const auto followedDate = res->followingSince->date();
                auto relativeTime = QString();
                if (getSettings()->showUsercardFollowageRelativeTime)
                {
                    relativeTime =
                        formatUsercardFollowRelativeTime(followedDate);
                }
                QString followingSince = followedDate.toString(Qt::ISODate);
                self->ui_.followageLabel->setText(
                    "Following since " + followingSince + relativeTime);
                self->ui_.followageLabel->setToolTip(
                    formatLongFriendlyDuration(
                        *res->followingSince, QDateTime::currentDateTimeUtc()) +
                    u" ago"_s);
                self->ui_.followageLabel->setMouseTracking(true);
                self->updateUsercardStatusIcons();
                self->ui_.followageRow->setVisible(true);
                self->ui_.followageIcon->setVisible(true);
            }
            else if (getSettings()->showUsercardFollowage)
            {
                self->ui_.followageLabel->setText({});
                self->ui_.followageRow->setVisible(true);
                self->ui_.followageIcon->setVisible(false);
            }

            if (getSettings()->showUsercardSubage && res->subscriptionMonths)
            {
                auto subageText = QString("Subscribed for %1 months")
                                      .arg(*res->subscriptionMonths);
                if (getSettings()->showUsercardSubageRelativeTime)
                {
                    subageText +=
                        formatUsercardYearsMonths(*res->subscriptionMonths);
                }
                self->ui_.subageLabel->setText(subageText);
                self->updateUsercardStatusIcons();
                self->ui_.subageRow->setVisible(true);
                self->ui_.subageIcon->setVisible(true);
            }
            else if (getSettings()->showUsercardSubage)
            {
                self->ui_.subageLabel->setText({});
                self->ui_.subageRow->setVisible(true);
                self->ui_.subageIcon->setVisible(false);
            }

            self->hideUsercardSubGiftRow();
        });

    this->ui_.block->setEnabled(false);
    this->ui_.ignoreHighlights->setEnabled(false);
    this->ui_.notesAdd->setEnabled(false);

    bool isMyself = false;  // FIXME: kick account
    this->ui_.block->setVisible(!isMyself);
    this->ui_.ignoreHighlights->setVisible(!isMyself);
}

void UserInfoPopup::onKickProfilePictureClick(Qt::MouseButton button)
{
    assert(this->isKick_);
    auto channelURL = QUrl("https://kick.com/" + this->kickUserSlug_);

    switch (button)
    {
        case Qt::LeftButton: {
            QDesktopServices::openUrl(channelURL);
        }
        break;

        // largely the same as on Twitch
        case Qt::RightButton: {
            if (this->avatarUrl_.isEmpty())
            {
                return;
            }

            auto *menu = new QMenu(this);
            menu->setAttribute(Qt::WA_DeleteOnClose);

            auto avatarUrl = this->avatarUrl_;

            // add context menu actions
            menu->addAction("Open &avatar in browser", this, [avatarUrl] {
                QDesktopServices::openUrl(QUrl(avatarUrl));
            });

            menu->addAction("Copy a&vatar link", this, [avatarUrl] {
                crossPlatformCopy(avatarUrl);
            });

            // we need to assign login name for msvc compilation
            auto username = this->userName_.toLower();
            menu->addAction(
                "Open channel in a new &popup window", this, [username] {
                    auto *app = getApp();
                    auto *split = app->getWindows()
                                      ->createWindow(WindowType::Popup, true)
                                      .getNotebook()
                                      .getOrAddSelectedPage()
                                      ->appendNewSplit(false);
                    split->setChannel(
                        app->getKickChatServer()->getOrCreate(username));
                });

            menu->addAction("Open channel in a new &tab", this, [username] {
                SplitContainer *container = getApp()
                                                ->getWindows()
                                                ->getMainWindow()
                                                .getNotebook()
                                                .addPage(true);
                auto *split = new Split(container);
                split->setChannel(
                    getApp()->getKickChatServer()->getOrCreate(username));
                container->insertSplit(split);
            });

            menu->addAction("Open channel in &browser", this, [channelURL] {
                QDesktopServices::openUrl(channelURL);
            });

            this->appendCommonProfileActions(menu);

            menu->popup(QCursor::pos());
            menu->raise();
        }
        break;

        default:
            break;
    }
}

QString UserInfoPopup::showProfilePictureContextMenu()
{
    if (this->avatarUrl_.isEmpty())
    {
        return "No avatar is available for this user.";
    }

    if (this->isKick_)
    {
        this->onKickProfilePictureClick(Qt::RightButton);
        return {};
    }

    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    auto avatarUrl = this->avatarUrl_;
    auto channelURL =
        QUrl("https://www.twitch.tv/" + this->userName_.toLower());

    menu->addAction("Open &avatar in browser", this, [avatarUrl] {
        QDesktopServices::openUrl(QUrl(avatarUrl));
    });

    menu->addAction("Copy a&vatar link", this, [avatarUrl] {
        crossPlatformCopy(avatarUrl);
    });

    auto loginName = this->userName_.toLower();
    menu->addAction("Open channel in a new &popup window", this, [loginName] {
        auto *app = getApp();
        auto &window = app->getWindows()->createWindow(WindowType::Popup, true);
        auto *split =
            window.getNotebook().getOrAddSelectedPage()->appendNewSplit(false);
        split->setChannel(app->getTwitch()->getOrAddChannel(loginName));
    });

    menu->addAction("Open channel in a new &tab", this, [loginName] {
        ChannelPtr channel = getApp()->getTwitch()->getOrAddChannel(loginName);
        auto &notebook = getApp()->getWindows()->getMainWindow().getNotebook();
        SplitContainer *container = notebook.addPage(true);
        Split *split = new Split(container);
        split->setChannel(channel);
        container->insertSplit(split);
    });

    menu->addAction("Open channel in &browser", this, [channelURL] {
        QDesktopServices::openUrl(channelURL);
    });

    this->appendCommonProfileActions(menu);

    menu->popup(QCursor::pos());
    menu->raise();
    return {};
}

bool UserInfoPopup::canShowRoleManagementMenu() const
{
    if (!getSettings()->showUsercardRoleManagementMenu || this->isKick_ ||
        this->userName_.isEmpty() || !this->underlyingChannel_)
    {
        return false;
    }

    auto *twitchChannel =
        dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get());
    if (twitchChannel == nullptr || twitchChannel->roomId().isEmpty())
    {
        return false;
    }

    const bool isMyself =
        getApp()->getAccounts()->twitch.getCurrent()->getUserName().compare(
            this->userName_, Qt::CaseInsensitive) == 0;
    const bool isChannelOwner =
        this->userName_.compare(twitchChannel->getName(),
                                Qt::CaseInsensitive) == 0;
    if (isMyself || isChannelOwner || this->isBroadcaster_)
    {
        return false;
    }

    const auto auth = MoltorinoAuth::resolveSavedBroadcasterToken(
        twitchChannel->roomId(), twitchChannel->getName());
    return auth.hasToken();
}

void UserInfoPopup::showRoleManagementMenu()
{
    if (this->ui_.rolesLabel == nullptr || !this->canShowRoleManagementMenu())
    {
        return;
    }

    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    const auto addAction = [this, menu](const QString &title,
                                        const QString &command,
                                        const QString &actionText) {
        menu->addAction(title, this, [this, command, actionText] {
            this->runRoleManagementCommand(command, actionText);
        });
    };

    addAction("Add lead moderator", "/leadmod", "add lead moderator to");
    addAction("Remove lead moderator", "/unleadmod",
              "remove lead moderator from");
    menu->addSeparator();
    addAction("Add editor", "/editor", "add editor to");
    addAction("Remove editor", "/uneditor", "remove editor from");

    menu->popup(this->ui_.rolesLabel->mapToGlobal(
        QPoint(0, this->ui_.rolesLabel->height())));
    menu->raise();
}

void UserInfoPopup::runRoleManagementCommand(const QString &command,
                                             const QString &actionText)
{
    if (!this->underlyingChannel_ || this->userName_.isEmpty())
    {
        return;
    }

    const bool wasPinned = this->ensurePinned();
    auto reply = QMessageBox::warning(
        this, "Confirm role change",
        QString("Are you sure you want to %1 %2 in #%3?")
            .arg(actionText, this->userName_,
                 this->underlyingChannel_->getName()),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (wasPinned)
    {
        this->togglePinned();
    }
    if (reply != QMessageBox::Yes)
    {
        return;
    }

    auto value = command + ' ' + this->userName_;
    value = getApp()->getCommands()->execCommand(
        value, this->underlyingChannel_, false);
    if (!value.isEmpty())
    {
        this->underlyingChannel_->sendMessage(value);
    }
}

void UserInfoPopup::updateUsercardStatusIcons()
{
    const auto boxSize = std::max(1, qRound(15 * this->scale()));
    const auto iconSize = std::max(1, qRound(14 * this->scale()));
    const bool isLight = getApp()->getThemes()->isLightTheme();
    const auto iconScale = this->devicePixelRatioF();

    auto updateIcon = [boxSize, iconScale](QLabel *label, const QString &path,
                                           int iconSize) {
        if (label == nullptr)
        {
            return;
        }

        label->setFixedSize(boxSize, boxSize);
        label->setAlignment(Qt::AlignCenter);
        label->setPixmap(renderUsercardStatusIcon(path, iconSize, iconScale));
    };

    auto updateColorSwatch = [this] {
        if (this->ui_.userColorSwatch == nullptr ||
            this->ui_.userColorRow == nullptr)
        {
            return;
        }

        auto colorText =
            this->ui_.userColorRow->property("copy-color").toString();
        if (colorText.isEmpty())
        {
            colorText =
                this->ui_.userColorRow->property("swatch-color").toString();
        }

        const QColor color(colorText);
        const auto swatchSize = std::max(1, qRound(8 * this->scale()));
        this->ui_.userColorSwatch->setFixedSize(swatchSize, swatchSize);
        if (color.isValid())
        {
            this->ui_.userColorSwatch->setStyleSheet(
                QString("QFrame#UsercardColorSwatch { background: %1; "
                        "border-radius: %2px; }")
                    .arg(color.name(QColor::HexRgb))
                    .arg(std::max(1, qRound(2 * this->scale()))));
        }
        else
        {
            this->ui_.userColorSwatch->setStyleSheet({});
        }
    };

    updateIcon(this->ui_.followageIcon,
               isLight ? ":/buttons/usercardFollow-lightMode.svg"
                       : ":/buttons/usercardFollow-darkMode.svg",
               iconSize);
    updateIcon(this->ui_.subageIcon,
               isLight ? ":/buttons/usercardSub-lightMode.svg"
                       : ":/buttons/usercardSub-darkMode.svg",
               iconSize);
    updateIcon(this->ui_.subGiftIcon,
               isLight ? ":/buttons/usercardGift-lightMode.svg"
                       : ":/buttons/usercardGift-darkMode.svg",
               iconSize);
    updateColorSwatch();
}

void UserInfoPopup::updateUsercardSubGiftRow(const IvrSubage &subageInfo)
{
    if (!getSettings()->showUsercardSubGiftGifter || !subageInfo.isSubbed ||
        !subageInfo.isGiftSub())
    {
        this->hideUsercardSubGiftRow();
        return;
    }

    const auto gifterName = subageInfo.giftGifterName();
    this->ui_.subGiftLabel->setText(
        gifterName.isEmpty() ? QString("Gifted by an anonymous user")
                             : QString("Gifted by %1").arg(gifterName));
    this->updateUsercardStatusIcons();
    this->ui_.subGiftRow->setVisible(true);
    this->ui_.subGiftIcon->setVisible(true);
}

void UserInfoPopup::hideUsercardSubGiftRow()
{
    this->ui_.subGiftLabel->setText({});
    this->ui_.subGiftLabel->setToolTip({});
    this->ui_.subGiftRow->setVisible(false);
    this->ui_.subGiftIcon->setVisible(false);
}

void UserInfoPopup::resetUsercardInfoRows()
{
    auto *settings = getSettings();
    const bool showTwitchProfileRows = !this->isKick_;

    this->ui_.followerCountLabel->setText(TEXT_FOLLOWERS.arg(""));
    this->ui_.followerCountLabel->setVisible(
        settings->showUsercardFollowerCount);

    this->ui_.createdDateLabel->setText(TEXT_CREATED.arg(""));
    this->ui_.createdDateLabel->setToolTip({});
    this->ui_.createdDateLabel->setVisible(settings->showUsercardCreatedDate);

    this->ui_.followageLabel->setText({});
    this->ui_.followageLabel->setToolTip({});
    this->ui_.followageRow->setVisible(settings->showUsercardFollowage);
    this->ui_.followageIcon->setVisible(false);

    this->ui_.subageLabel->setText({});
    this->ui_.subageRow->setVisible(settings->showUsercardSubage);
    this->ui_.subageIcon->setVisible(false);

    this->hideUsercardSubGiftRow();

    this->ui_.chatterCountLabel->setText("Chatters: ...");
    this->ui_.chatterCountLabel->setVisible(
        showTwitchProfileRows && settings->showUsercardChatterCount &&
        settings->showUserinfoPopupChatters.getValue());
    this->ui_.lastLiveLabel->setText("Last live: 0000-00-00");
    this->ui_.lastLiveLabel->setToolTip({});
    this->ui_.lastLiveLabel->setVisible(
        showTwitchProfileRows && settings->showUsercardLastLive &&
        settings->showUserinfoPopupLastLive.getValue());
    this->ui_.userColorLabel->setText("Color: #FFFFFF");
    this->ui_.userColorRow->setProperty("copy-color", {});
    this->ui_.userColorRow->setProperty("swatch-color", "#FFFFFF");
    this->ui_.userColorSwatch->setStyleSheet(
        "QFrame#UsercardColorSwatch { background: #FFFFFF; "
        "border-radius: 2px; }");
    this->ui_.userColorRow->setVisible(
        showTwitchProfileRows && settings->showUsercardColor &&
        settings->showUserinfoPopupColor.getValue());
    this->ui_.statusLabel->setText("Status: ...");
    this->ui_.statusLabel->setVisible(showTwitchProfileRows &&
                                      settings->showUsercardStatus);
    this->ui_.bannedAvatarLabel->hide();

    this->isUserLive_ = false;
    this->liveViewerCount_ = 0;
    if (this->ui_.liveIndicator)
    {
        this->ui_.liveIndicator->hide();
    }

    this->updateUsercardStatusIcons();
    this->refreshSeventvPaint();
}

void UserInfoPopup::applyIvrUserProfile(const IvrUserProfile &profile)
{
    auto *settings = getSettings();

    this->ui_.bannedAvatarLabel->setVisible(profile.banned);

    if (settings->showUsercardChatterCount &&
        settings->showUserinfoPopupChatters.getValue())
    {
        this->ui_.chatterCountLabel->setText(
            profile.chatterCount
                ? "Chatters: " + localizeNumbers(*profile.chatterCount)
                : "Chatters: " % TEXT_UNAVAILABLE);
        this->ui_.chatterCountLabel->setVisible(true);
    }

    if (settings->showUsercardLastLive &&
        settings->showUserinfoPopupLastLive.getValue())
    {
        const auto lastLive = formatIvrDate(profile.lastBroadcastStartedAt);
        if (lastLive.isEmpty())
        {
            this->ui_.lastLiveLabel->setText("Last live: " % TEXT_UNAVAILABLE);
            this->ui_.lastLiveLabel->setToolTip({});
        }
        else
        {
            this->ui_.lastLiveLabel->setText("Last live: " + lastLive);
            if (!profile.lastBroadcastTitle.isEmpty())
            {
                this->ui_.lastLiveLabel->setToolTip(profile.lastBroadcastTitle);
                this->ui_.lastLiveLabel->setMouseTracking(true);
            }
        }
        this->ui_.lastLiveLabel->setVisible(true);
    }

    if (settings->showUsercardColor &&
        settings->showUserinfoPopupColor.getValue())
    {
        const QColor color(profile.chatColor);
        if (color.isValid())
        {
            const auto colorHex = color.name(QColor::HexRgb).toUpper();
            this->ui_.userColorRow->setProperty("copy-color", colorHex);
            this->ui_.userColorRow->setProperty("swatch-color", colorHex);
            this->ui_.userColorLabel->setText("Color: " + colorHex);
            this->updateUsercardStatusIcons();
            this->ui_.userColorRow->setVisible(true);
            this->updateSeventvPaintPixmap();
        }
        else
        {
            this->ui_.userColorRow->setProperty("copy-color", {});
            this->ui_.userColorRow->setProperty("swatch-color", {});
            this->ui_.userColorLabel->setText("Color: " % TEXT_UNAVAILABLE);
            this->ui_.userColorRow->setVisible(true);
            this->updateSeventvPaintPixmap();
        }
    }

    if (settings->showUsercardStatus)
    {
        this->ui_.statusLabel->setText("Status: " +
                                       formatUsercardStatus(profile));
        this->ui_.statusLabel->setVisible(true);
    }
}

void UserInfoPopup::refreshSevenTVUserButtonVisibility()
{
    if (this->ui_.sevenTVUserLabel == nullptr)
    {
        return;
    }

    const bool settingEnabled = getSettings()->showSevenTVUsercardButton;
    const bool hasSevenTVUser = !this->seventvUserID_.isEmpty();
    const bool lookupPending =
        !this->seventvUserLookupFinished_ && !hasSevenTVUser;
    const bool shouldShow = settingEnabled && (lookupPending || hasSevenTVUser);

    this->ui_.sevenTVUserLabel->setVisible(shouldShow);
    this->ui_.sevenTVUserLabel->setEnabled(settingEnabled && hasSevenTVUser);

    if (hasSevenTVUser)
    {
        this->ui_.sevenTVUserLabel->setToolTip("Open 7TV profile");
    }
    else if (this->seventvUserLookupInFlight_)
    {
        this->ui_.sevenTVUserLabel->setToolTip("Checking 7TV profile...");
    }
    else if (this->seventvUserLookupFinished_)
    {
        this->ui_.sevenTVUserLabel->setToolTip("No 7TV profile found");
    }
    else
    {
        this->ui_.sevenTVUserLabel->setToolTip("7TV profile not loaded yet");
    }
}

void UserInfoPopup::resetNameHistory()
{
    ++this->nameHistoryRequestGeneration_;
    if (this->nameHistoryMenu_ != nullptr)
    {
        this->nameHistoryMenu_->close();
        this->nameHistoryMenu_ = nullptr;
    }
    this->nameHistoryLogin_.clear();
    this->nameHistoryEntries_.clear();
    this->nameHistoryLoading_ = false;
    this->nameHistoryLoaded_ = false;
    this->applyCachedNameHistory();
    this->updateNameHistoryButton();
}

bool UserInfoPopup::applyCachedNameHistory()
{
    if (this->userName_.isEmpty() || this->userId_.isEmpty() || this->isKick_)
    {
        return false;
    }

    const auto login = normalizeTwitchNameHistoryLogin(this->userName_);
    if (login.isEmpty())
    {
        return false;
    }

    auto cached = getCachedTwitchNameHistory(this->userId_, login);
    if (!cached)
    {
        return false;
    }

    this->nameHistoryLogin_ = login;
    this->nameHistoryEntries_ = cached->entries;
    this->nameHistoryLoading_ = false;
    this->nameHistoryLoaded_ = true;
    return true;
}

void UserInfoPopup::updateNameHistoryButton()
{
    if (this->ui_.nameHistoryButton == nullptr)
    {
        return;
    }

    const bool canShow = getSettings()->showUsercardNameHistoryButton &&
                         !this->isKick_ && !this->userName_.isEmpty();
    this->ui_.nameHistoryButton->setVisible(canShow);
    this->ui_.nameHistoryButton->setEnabled(canShow &&
                                            !this->userId_.isEmpty());
    this->ui_.nameHistoryButton->setText(this->nameHistoryLoading_ ? "..."
                                                                   : "aka");

    if (!canShow)
    {
        if (this->nameHistoryMenu_ != nullptr)
        {
            this->nameHistoryMenu_->close();
            this->nameHistoryMenu_ = nullptr;
        }
        this->ui_.nameHistoryButton->setToolTip({});
        return;
    }
    if (this->userId_.isEmpty())
    {
        this->ui_.nameHistoryButton->setToolTip(
            "Name history loads after Twitch profile data.");
        return;
    }
    if (this->nameHistoryLoading_)
    {
        this->ui_.nameHistoryButton->setToolTip("Fetching name history...");
        return;
    }
    const auto login = normalizeTwitchNameHistoryLogin(this->userName_);
    if (this->nameHistoryLoaded_ && this->nameHistoryLogin_ == login &&
        this->nameHistoryEntries_.empty())
    {
        this->ui_.nameHistoryButton->setToolTip("No name history found");
        return;
    }

    this->ui_.nameHistoryButton->setToolTip("Show name history");
}

void UserInfoPopup::showNameHistoryMenu()
{
    if (this->ui_.nameHistoryButton == nullptr || this->userName_.isEmpty() ||
        this->userId_.isEmpty() || this->isKick_)
    {
        return;
    }
    if (this->nameHistoryLoading_)
    {
        this->openNameHistoryMenu("Fetching name history...");
        return;
    }

    if (this->applyCachedNameHistory())
    {
        this->updateNameHistoryButton();
        this->openNameHistoryMenu();
        return;
    }

    const auto login = normalizeTwitchNameHistoryLogin(this->userName_);
    if (this->nameHistoryLoaded_ && this->nameHistoryLogin_ == login)
    {
        this->openNameHistoryMenu();
        return;
    }

    this->requestNameHistory();
}

void UserInfoPopup::openNameHistoryMenu(const QString &statusText)
{
    auto *button = this->ui_.nameHistoryButton;
    if (button == nullptr || !button->isVisible())
    {
        return;
    }

    if (this->nameHistoryMenu_ != nullptr)
    {
        this->nameHistoryMenu_->close();
    }

    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    this->nameHistoryMenu_ = menu;

    if (!statusText.isEmpty())
    {
        auto *status = menu->addAction(statusText);
        status->setEnabled(false);
    }
    else if (this->nameHistoryEntries_.empty())
    {
        auto *empty = menu->addAction("No name history found");
        empty->setEnabled(false);
    }
    else
    {
        for (const auto &entry : this->nameHistoryEntries_)
        {
            auto *action = new QWidgetAction(menu);
            action->setDefaultWidget(new NameHistoryMenuRow(
                entry.login, entry.leftText, entry.rightText, menu));
            menu->addAction(action);
        }

        if (static_cast<int>(this->nameHistoryEntries_.size()) >=
            TWITCH_NAME_HISTORY_LIMIT)
        {
            menu->addSeparator();
            auto *limited =
                menu->addAction(QString("Showing latest %1 names")
                                    .arg(TWITCH_NAME_HISTORY_LIMIT));
            limited->setEnabled(false);
        }
    }

    menu->popup(button->mapToGlobal(QPoint(0, button->height())));
}

void UserInfoPopup::requestNameHistory()
{
    if (this->userName_.isEmpty() || this->userId_.isEmpty() || this->isKick_)
    {
        return;
    }

    const auto login = normalizeTwitchNameHistoryLogin(this->userName_);
    if (login.isEmpty())
    {
        return;
    }

    const auto generation = ++this->nameHistoryRequestGeneration_;
    const auto userId = this->userId_;
    this->nameHistoryLogin_ = login;
    this->nameHistoryEntries_.clear();
    this->nameHistoryLoading_ = true;
    this->nameHistoryLoaded_ = false;
    this->updateNameHistoryButton();
    this->openNameHistoryMenu("Fetching name history...");

    const QPointer<UserInfoPopup> self(this);

    fetchTwitchNameHistoryByUserId(
        userId, login,
        [self, generation, userId, login](TwitchNameHistory history) mutable {
            if (!self || generation != self->nameHistoryRequestGeneration_ ||
                self->userId_ != userId ||
                normalizeTwitchNameHistoryLogin(self->userName_) != login)
            {
                return;
            }

            self->nameHistoryLogin_ = login;
            self->nameHistoryEntries_ = std::move(history.entries);
            self->nameHistoryLoading_ = false;
            self->nameHistoryLoaded_ = true;
            self->updateNameHistoryButton();
            self->openNameHistoryMenu();
        },
        [self, generation, userId, login](const QString &error) {
            if (!self || generation != self->nameHistoryRequestGeneration_ ||
                self->userId_ != userId ||
                normalizeTwitchNameHistoryLogin(self->userName_) != login)
            {
                return;
            }

            qCWarning(chatterinoWidget)
                << "Failed to fetch name history:" << error;
            self->nameHistoryEntries_.clear();
            self->nameHistoryLoading_ = false;
            self->nameHistoryLoaded_ = false;
            self->updateNameHistoryButton();
            self->openNameHistoryMenu("Name history unavailable");
        });
}

QStringView UserInfoPopup::platformName() const
{
    if (this->isKick_)
    {
        return u"Kick";
    }
    return u"Twitch";
}

void UserInfoPopup::appendCommonProfileActions(QMenu *menu)
{
    if (!this->isKick_ && !this->userName_.isEmpty())
    {
        menu->addAction("Open profile in &ChatVault", this,
                        [url = chatVaultTwitchChannelUrl(this->userName_)] {
                            QDesktopServices::openUrl(QUrl(url));
                        });

        menu->addAction("Open channel &logs in browser", this,
                        [username = this->userName_] {
                            QDesktopServices::openUrl(
                                QUrl("https://tv.supa.sh/logs?c=" + username));
                        });
    }

    if (!this->seventvUserID_.isEmpty())
    {
        menu->addAction(
            "Open &7TV user in browser", this, [id = this->seventvUserID_] {
                QDesktopServices::openUrl(QUrl(SEVENTV_USER_PAGE % id));
            });
    }
}

void UserInfoPopup::executeUsercardModerationAction(
    const UsercardModerationRequest &request)
{
    if (!this->underlyingChannel_)
    {
        return;
    }

    QString value;
    switch (request.action)
    {
        case UsercardModerationAction::Ban: {
            value = appendModerationReason("/ban " + this->userName_,
                                           request.reason);
        }
        break;

        case UsercardModerationAction::Unban: {
            value = "/unban " + this->userName_;
        }
        break;

        case UsercardModerationAction::Timeout: {
            if (request.durationSeconds <= 0)
            {
                return;
            }

            value = appendModerationReason(
                "/timeout " + this->userName_ + " " +
                    QString::number(request.durationSeconds) + 's',
                request.reason);
        }
        break;
    }

    value = getApp()->getCommands()->execCommand(
        value, this->underlyingChannel_, false);
    this->underlyingChannel_->sendMessage(value);
}

void UserInfoPopup::showUsercardModerationReasonPopup(
    const UsercardModerationRequest &request)
{
    if (request.action == UsercardModerationAction::Unban)
    {
        this->executeUsercardModerationAction(request);
        return;
    }

    if (this->moderationReasonPopup_)
    {
        this->moderationReasonPopup_->close();
    }

    const bool wasPinned = this->ensurePinned();

    const auto isBan = request.action == UsercardModerationAction::Ban;
    const auto initialReason =
        getSettings()->timeoutReasonPromptPrefillSavedReason.getValue()
            ? request.reason
            : QString();
    auto *popup = new ModerationReasonPopup(
        isBan ? "Ban reason" : "Timeout reason", "optional reason",
        initialReason,
        getSettings()->timeoutReasonPromptShowSendButton.getValue(),
        [self = QPointer<UserInfoPopup>(this),
         request](QString reason) mutable {
            if (!self)
            {
                return;
            }

            auto updatedRequest = request;
            updatedRequest.reason = reason;
            updatedRequest.promptForReason = false;
            self->executeUsercardModerationAction(updatedRequest);
        },
        this);

    popup->setAttribute(Qt::WA_DeleteOnClose);
    this->moderationReasonPopup_ = popup;
    if (wasPinned)
    {
        auto didRestorePin = std::make_shared<bool>(false);
        auto restorePin = [self = QPointer<UserInfoPopup>(this),
                           didRestorePin] {
            if (*didRestorePin)
            {
                return;
            }
            *didRestorePin = true;

            if (self)
            {
                self->togglePinned();
            }
        };
        std::ignore = popup->closing.connect(restorePin);
        QObject::connect(popup, &QObject::destroyed, this,
                         std::move(restorePin));
    }
    popup->showCenteredAt(QCursor::pos());
    popup->activateWindow();
    popup->raise();
}

//
// TimeoutWidget
//
UserInfoPopup::TimeoutWidget::TimeoutWidget()
    : BaseWidget(nullptr)
{
    auto layout = LayoutCreator<TimeoutWidget>(this)
                      .setLayoutType<QHBoxLayout>()
                      .withoutMargin();

    int buttonWidth = 40;
    int buttonHeight = 32;

    layout->setSpacing(16);

    const auto addLayout = [&](const QString &text) {
        auto vbox = layout.emplace<QVBoxLayout>().withoutMargin();
        auto title = vbox.emplace<QHBoxLayout>().withoutMargin();
        title->addStretch(1);
        auto label = title.emplace<Label>(text);
        label->setStyleSheet("color: #BBB");
        label->setPadding(QMargins{});
        title->addStretch(1);

        auto hbox = vbox.emplace<QHBoxLayout>().withoutMargin();
        hbox->setSpacing(0);
        return hbox;
    };

    const auto addButton = [&](UsercardModerationAction action,
                               const QString &title, const QPixmap &pixmap) {
        auto button = addLayout(title).emplace<PixmapButton>(nullptr);
        button->setPixmap(pixmap);
        button->setScaleIndependentSize(buttonHeight, buttonHeight);
        button->setBorderColor(QColor(255, 255, 255, 127));
        if (action != UsercardModerationAction::Unban)
        {
            button->setToolTip(
                "Use the configured reason prompt shortcut to edit the reason "
                "before sending.");
        }

        QObject::connect(button.getElement(), &Button::clicked,
                         [this, action](Qt::MouseButton button) {
                             if (!shouldHandleModerationButtonClick(button))
                             {
                                 return;
                             }

                             UsercardModerationRequest request;
                             request.action = action;
                             if (action == UsercardModerationAction::Ban)
                             {
                                 request.reason = timeoutBanReason();
                             }
                             request.promptForReason =
                                 action != UsercardModerationAction::Unban &&
                                 shouldPromptForModerationReason(button);

                             this->buttonClicked.invoke(request);
                         });
    };

    auto addTimeouts = [&](const QString &title) {
        auto hbox = addLayout(title);

        int index = 0;
        for (const auto &item : getSettings()->timeoutButtons.getValue())
        {
            auto a = hbox.emplace<LabelButton>();
            a->setPadding({0, 0});
            a->setText(QString::number(item.second) + item.first);

            a->setScaleIndependentSize(buttonWidth, buttonHeight);
            a->setBorderColor(borderColor);

            const auto duration = calculateTimeoutDuration(item);
            const auto reason = timeoutButtonReason(index);
            this->timeoutButtons.emplace_back(a.getElement(), duration);

            QObject::connect(a.getElement(), &LabelButton::clicked,
                             [this, duration, reason](Qt::MouseButton button) {
                                 if (!shouldHandleModerationButtonClick(button))
                                 {
                                     return;
                                 }

                                 UsercardModerationRequest request;
                                 request.action =
                                     UsercardModerationAction::Timeout;
                                 request.durationSeconds = duration;
                                 request.reason = reason;
                                 request.promptForReason =
                                     shouldPromptForModerationReason(button);

                                 this->buttonClicked.invoke(request);
                             });
            a->setToolTip(
                "Use the configured reason prompt shortcut to edit the reason "
                "before sending.");
            ++index;
        }
    };

    addButton(UsercardModerationAction::Unban, "Unban",
              getResources().buttons.unban);
    addTimeouts("Timeouts");
    addButton(UsercardModerationAction::Ban, "Ban", getResources().buttons.ban);
}

void UserInfoPopup::TimeoutWidget::paintEvent(QPaintEvent *)
{
    //    QPainter painter(this);

    //    painter.setPen(QColor(255, 255, 255, 63));

    //    painter.drawLine(0, this->height() / 2, this->width(), this->height()
    //    / 2);
}

void UserInfoPopup::TimeoutWidget::setMinTimeout(int minSecs)
{
    for (auto &[widget, dur] : this->timeoutButtons)
    {
        widget->setVisible(dur >= minSecs);
    }
}

void UserInfoPopup::updateAvatarUrl()
{
    if (this->isTwitchAvatarShown_)
    {
        this->avatarUrl_ = this->helixAvatarUrl_;
    }
    else
    {
        this->avatarUrl_ = this->seventvAvatarUrl_;
    }
}

void UserInfoPopup::refreshSeventvPaint()
{
    if (!getSettings()->showUsercardSevenTVPaint)
    {
        this->seventvPaint_ = nullptr;
        if (this->ui_.seventvPaintRow)
        {
            this->ui_.seventvPaintRow->hide();
        }
        return;
    }

    const auto paint = getApp()->getSeventvPaints()->getPaint(
        this->userName_.toLower(), this->isKick_);
    this->seventvPaint_ = paint;

    if (!paint)
    {
        this->ui_.seventvPaintRow->hide();
        return;
    }

    this->ui_.seventvPaintRow->show();
    this->updateSeventvPaintPixmap();
}

void UserInfoPopup::updateSeventvPaintPixmap()
{
    const auto paint = this->seventvPaint_;
    if (!paint || !this->ui_.seventvPaintPixmapLabel)
    {
        return;
    }

    const auto font =
        getApp()->getFonts()->getFont(FontStyle::UiMedium, this->scale());
    const auto dpr = this->devicePixelRatioF();
    const QString paintName = paint->name.isEmpty() ? paint->id : paint->name;

    const QFontMetricsF metrics(font);
    const int lineHeight = std::max(1, qRound(metrics.height()));
    const int paintWidth =
        std::max(1, qRound(metrics.horizontalAdvance(paintName)));
    const QSizeF size(paintWidth, lineHeight);

    this->ui_.seventvPaintPixmapLabel->setFixedSize(paintWidth, lineHeight);

    QColor userColor = Qt::white;
    if (this->ui_.userColorRow)
    {
        const auto colorText =
            this->ui_.userColorRow->property("swatch-color").toString();
        if (!colorText.isEmpty())
        {
            userColor = QColor(colorText);
        }
    }

    const auto pixmap = paint->getPixmap(paintName, font, userColor, size,
                                         this->scale(), dpr, true);

    this->ui_.seventvPaintPixmapLabel->setPixmap(pixmap);
    this->ui_.seventvPaintPixmapLabel->setToolTip(paintName);
}

UserInfoPopup::~UserInfoPopup()
{
    unregisterActiveUsercard(this);
}

void UserInfoPopup::notifyFollowMutation(const QString &targetId,
                                         const QString &requestUserId,
                                         const QString &requestLogin,
                                         bool unfollow)
{
    auto current = getApp()->getAccounts()->twitch.getCurrent();
    bool accountMatches = false;
    if (current && !current->isAnon())
    {
        const auto currentUserId = current->getUserId().trimmed();
        const auto normalizedUserId = requestUserId.trimmed();
        if (!currentUserId.isEmpty() && !normalizedUserId.isEmpty())
        {
            accountMatches = normalizedUserId == currentUserId;
        }
        else
        {
            const auto currentLogin =
                current->getUserName().trimmed().toLower();
            const auto normalizedLogin = requestLogin.trimmed().toLower();
            accountMatches = !currentLogin.isEmpty() &&
                             !normalizedLogin.isEmpty() &&
                             normalizedLogin == currentLogin;
        }
    }

    if (!accountMatches)
    {
        return;
    }

    QMutexLocker locker(&activeUsercardsMutex());
    for (const auto &popup : activeUsercards())
    {
        if (!popup || popup->userId_ != targetId)
        {
            continue;
        }

        std::optional<QDateTime> followedAt;
        if (!unfollow)
        {
            followedAt = QDateTime::currentDateTimeUtc();
        }
        popup->setFollowingStatus(!unfollow, followedAt);
    }
}

bool UserInfoPopup::isFollowing() const
{
    return this->following_;
}

bool UserInfoPopup::isFollowingStatusKnown() const
{
    return this->followingStatusKnown_;
}

void UserInfoPopup::setFollowingStatus(bool following,
                                       std::optional<QDateTime> followedAt)
{
    const bool changed = this->followingStatusKnown_ != true ||
                         this->following_ != following ||
                         this->followedAt_ != followedAt;

    this->following_ = following;
    this->followingStatusKnown_ = true;
    this->followedAt_ = std::move(followedAt);
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (account && !account->isAnon())
    {
        this->followingStatusUserId_ = account->getUserId();
    }

    if (changed)
    {
        this->followingStatusChanged_.invoke();
    }
}

void UserInfoPopup::resetFollowingStatus()
{
    const bool changed = this->followingStatusKnown_ || this->following_;
    this->following_ = false;
    this->followingStatusKnown_ = false;
    this->followedAt_.reset();
    this->followingStatusUserId_.clear();
    this->followingStatusFetchInFlight_.store(false);

    if (changed)
    {
        this->followingStatusChanged_.invoke();
    }
}

void UserInfoPopup::refreshFollowingStatus(bool force)
{
    if (getApp()->isTest())
    {
        return;
    }

    const auto targetUserId = this->userId_;
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    const auto userId =
        account && !account->isAnon() ? account->getUserId() : QString();

    auto clearUnknown = [this] {
        const bool changed = this->followingStatusKnown_ || this->following_;
        this->following_ = false;
        this->followingStatusKnown_ = false;
        this->followedAt_.reset();
        this->followingStatusUserId_.clear();
        if (changed)
        {
            this->followingStatusChanged_.invoke();
        }
    };

    if (targetUserId.isEmpty() || userId.isEmpty())
    {
        clearUnknown();
        return;
    }

    if (userId == targetUserId)
    {
        this->followingStatusUserId_ = userId;
        this->setFollowingStatus(false);
        return;
    }

    if (!force && this->followingStatusKnown_ &&
        this->followingStatusUserId_ == userId)
    {
        return;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    if (!force && this->lastFollowingStatusRefreshAt_.isValid() &&
        this->lastFollowingStatusRefreshAt_.msecsTo(now) <
            FOLLOWING_STATUS_RETRY_INTERVAL_MS)
    {
        return;
    }

    if (this->followingStatusFetchInFlight_.exchange(true))
    {
        return;
    }

    this->lastFollowingStatusRefreshAt_ = now;
    const auto requestUserId = userId;
    const auto requestTargetUserId = targetUserId;
    QPointer<UserInfoPopup> self(this);

    getHelix()->getFollowedChannel(
        requestUserId, requestTargetUserId, this,
        [self, requestUserId, requestTargetUserId](const auto &chan) {
            if (!self)
            {
                return;
            }

            self->followingStatusFetchInFlight_.store(false);

            auto current = getApp()->getAccounts()->twitch.getCurrent();
            const auto currentUserId = current && !current->isAnon()
                                           ? current->getUserId()
                                           : QString();
            if (currentUserId != requestUserId ||
                self->userId_ != requestTargetUserId)
            {
                self->refreshFollowingStatus(true);
                return;
            }

            self->followingStatusUserId_ = requestUserId;
            self->setFollowingStatus(
                chan.has_value(),
                chan ? std::optional<QDateTime>(chan->followedAt)
                     : std::nullopt);
        },
        [self](const QString &error) {
            if (!self)
            {
                return;
            }

            self->followingStatusFetchInFlight_.store(false);
            qCDebug(chatterinoTwitch)
                << "Failed to refresh usercard following status for"
                << self->userName_ << ':' << error;
        });
}

void UserInfoPopup::updateLiveIndicatorDisplay()
{
    if (this->ui_.liveIndicator == nullptr)
    {
        return;
    }

    this->ui_.liveIndicator->setTextMode(
        getSettings()->showUsercardLiveViewerCount);

    if (this->isUserLive_)
    {
        this->ui_.liveIndicator->setViewers(this->liveViewerCount_);
        this->ui_.liveIndicator->show();
    }
    else
    {
        this->ui_.liveIndicator->hide();
    }
}

void UserInfoPopup::updateUsercardFollowButton()
{
    if (this->ui_.followButton == nullptr)
    {
        return;
    }

    if (!getSettings()->showFollowButtonInUsercard || this->isKick_ ||
        this->userId_.isEmpty() ||
        !canUseFollowButtonForUser(this->userId_, this->userName_))
    {
        this->ui_.followButton->hide();
        return;
    }

    const auto following =
        this->isFollowingStatusKnown() && this->isFollowing();
    const auto displayName =
        this->ui_.localizedNameLabel->isVisible() &&
                !this->ui_.localizedNameLabel->getText().isEmpty()
            ? this->ui_.localizedNameLabel->getText()
            : (this->ui_.nameLabel->getText().isEmpty()
                   ? this->userName_
                   : this->ui_.nameLabel->getText());

    this->ui_.followButton->setSource(followButtonSource(following));
    this->ui_.followButton->setToolTip(
        formatFollowButtonToolTip(displayName, following, this->followedAt_));
    const int buttonSize = std::max(1, int(28 * this->scale()));
    this->ui_.followButton->setFixedSize(buttonSize, buttonSize);
    this->ui_.followButton->show();
}

void UserInfoPopup::toggleUsercardFollow()
{
    if (this->isKick_ || this->userId_.isEmpty() || this->userName_.isEmpty())
    {
        return;
    }

    const auto command = this->isFollowingStatusKnown() && this->isFollowing()
                             ? QStringLiteral("/unfollow")
                             : QStringLiteral("/follow");

    if (command == QStringLiteral("/unfollow") &&
        getSettings()->confirmUnfollowFromUsercard)
    {
        const auto displayName =
            this->ui_.localizedNameLabel->isVisible() &&
                    !this->ui_.localizedNameLabel->getText().isEmpty()
                ? this->ui_.localizedNameLabel->getText()
                : (this->ui_.nameLabel->getText().isEmpty()
                       ? this->userName_
                       : this->ui_.nameLabel->getText());

        const bool wasPinned = this->ensurePinned();

        QMessageBox box(this);
        box.setWindowTitle("Unfollow channel?");
        box.setIcon(QMessageBox::Question);
        box.setText(
            QString("Are you sure you want to unfollow %1?").arg(displayName));

        auto *confirmButton =
            box.addButton("Confirm", QMessageBox::DestructiveRole);
        auto *cancelButton = box.addButton("Cancel", QMessageBox::RejectRole);
        box.setDefaultButton(cancelButton);
        box.setEscapeButton(cancelButton);
        box.exec();

        if (wasPinned)
        {
            this->togglePinned();
        }

        if (box.clickedButton() != confirmButton)
        {
            return;
        }
    }

    CommandContext ctx{
        .words = {command, this->userName_},
        .rawText = command % u' ' % this->userName_,
        .channel = this->channel_,
        .twitchChannel =
            dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get()),
        .kickChannel = nullptr,
    };
    const auto text = command == QStringLiteral("/unfollow")
                          ? commands::unfollow(ctx)
                          : commands::follow(ctx);
    if (!text.isEmpty())
    {
        this->channel_->sendMessage(text);
    }
}

}  // namespace chatterino

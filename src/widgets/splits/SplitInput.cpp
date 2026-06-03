// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/SplitInput.hpp"

#include "Application.hpp"
#include "common/enums/MessageOverflow.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/builtin/twitch/Nuke.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/completion/sources/CommandSource.hpp"
#include "controllers/completion/strategies/CommandStrategy.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/spellcheck/SpellChecker.hpp"
#include "messages/Link.hpp"
#include "messages/Message.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/translation/Translator.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Helpers.hpp"
#include "util/LayoutCreator.hpp"
#include "util/MultiChannel.hpp"
#include "widgets/buttons/LabelButton.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/dialogs/EmotePopup.hpp"
#include "widgets/dialogs/PollDialog.hpp"
#include "widgets/dialogs/PredictionDialog.hpp"
#include "widgets/dialogs/UserInfoPopup.hpp"
#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
#    include "widgets/dialogs/ChannelPointsDialog.hpp"
#endif
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/CmdDeleteKeyFilter.hpp"
#include "widgets/helper/MessageView.hpp"
#include "widgets/helper/ResizingTextEdit.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/Scrollbar.hpp"
#include "widgets/splits/InputCompletionPopup.hpp"
#include "widgets/splits/InputHighlighter.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"

#include <QActionGroup>
#include <QCompleter>
#include <QDateTime>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QWheelEvent>

#include <algorithm>
#include <functional>
#include <limits>
#include <ranges>

using namespace Qt::Literals::StringLiterals;

namespace chatterino {

namespace {

constexpr int RAID_STATUS_PROGRESS_RANGE = 100'000;
constexpr int RAID_STATUS_PROGRESS_TICK_MS = 1000;
constexpr qint64 CHANNEL_POINTS_MANUAL_REFRESH_COOLDOWN_MS = 10'000;
constexpr int OUTGOING_TRANSLATION_DEBOUNCE_MS = 450;
constexpr auto WARNING_MESSAGE_TARGET_LOGIN = "msg";
constexpr auto OUTGOING_TRANSLATION_OFF = "off";
constexpr auto OUTGOING_TRANSLATION_PREVIEW = "preview";
constexpr auto OUTGOING_TRANSLATION_SEND = "send";

// Current function: https://www.desmos.com/calculator/vdyamchjwh
qreal highlightEasingFunction(qreal progress)
{
    if (progress <= 0.1)
    {
        return 1.0 - pow(10.0 * progress, 3.0);
    }
    return 1.0 + pow((20.0 / 9.0) * (0.5 * progress - 0.5), 3.0);
}

int compactWidgetWidth(QWidget *widget)
{
    if (widget == nullptr)
    {
        return 0;
    }

    auto hint = widget->sizeHint();
    if (!hint.isValid())
    {
        hint = widget->minimumSizeHint();
    }

    return std::max(0, hint.width());
}

void makeInputActionLabelCompressible(QLabel *label)
{
    label->setMinimumWidth(0);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
}

void makeInputActionLabelSticky(QLabel *label)
{
    label->setMinimumWidth(0);
    label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void updateInputActionLabelMinimumWidth(QLabel *label)
{
    if (label == nullptr)
    {
        return;
    }

    label->setMinimumWidth(label->text().isEmpty() ? 0
                                                   : compactWidgetWidth(label));
}

QString formatChannelPointsValue(qint64 balance)
{
    if (balance < 0)
    {
        return QStringLiteral("...");
    }

    return formatCompactNumber(balance);
}

QString formatChannelPointsToolTip(qint64 balance)
{
    const auto action =
#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
        getSettings()->openRewardsWithChannelPointsClick
            ? QStringLiteral("Click to open rewards")
            : QStringLiteral("Click to refresh");
#else
        QStringLiteral("Click to refresh");
#endif

    if (balance >= 0)
    {
        const auto locale = getSystemLocale();
        return QStringLiteral("Channel Points: %1\n%2")
            .arg(locale.toString(balance), action);
    }

    return QStringLiteral("Channel Points: loading...\n%1").arg(action);
}

QString channelPointsIdleToolTip()
{
#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
    if (getSettings()->openRewardsWithChannelPointsClick)
    {
        return QStringLiteral("Channel Points (click to open rewards)");
    }
#endif
    return QStringLiteral("Channel Points (click to refresh)");
}

QString normalizedOutgoingTranslationMode(QString mode)
{
    mode = mode.trimmed().toLower();
    if (mode == QLatin1String(OUTGOING_TRANSLATION_PREVIEW) ||
        mode == QLatin1String(OUTGOING_TRANSLATION_SEND))
    {
        return mode;
    }
    return QStringLiteral("off");
}

QString outgoingTranslationModeLabel(const QString &mode)
{
    const auto normalized = normalizedOutgoingTranslationMode(mode);
    if (normalized == QLatin1String(OUTGOING_TRANSLATION_PREVIEW))
    {
        return QStringLiteral("Preview only");
    }
    if (normalized == QLatin1String(OUTGOING_TRANSLATION_SEND))
    {
        return QStringLiteral("Translate on send");
    }
    return QStringLiteral("Off");
}

QString formatRaidCountdown(int seconds)
{
    seconds = std::max(0, seconds);
    const auto minutes = seconds / 60;
    const auto secs = seconds % 60;
    return QStringLiteral("%1:%2").arg(minutes).arg(secs, 2, 10,
                                                    QLatin1Char('0'));
}

qint64 remainingRaidMilliseconds(const TwitchChannel::RaidEvent &raid)
{
    const auto totalSeconds = std::max(1, raid.forceRaidNowSeconds);
    auto startedAt =
        raid.raidCreatedAt.isValid() ? raid.raidCreatedAt : raid.receivedAt;
    if (!startedAt.isValid())
    {
        startedAt = QDateTime::currentDateTimeUtc();
    }

    const auto deadline = startedAt.addSecs(totalSeconds);
    return QDateTime::currentDateTimeUtc().msecsTo(deadline);
}

QString raidViewerText(int viewerCount)
{
    return QStringLiteral("%1 viewer%2")
        .arg(std::max(0, viewerCount))
        .arg(viewerCount == 1 ? QString() : QStringLiteral("s"));
}

int raidProgressValue(qint64 elapsedMs, qint64 totalMs)
{
    if (totalMs <= 0)
    {
        return RAID_STATUS_PROGRESS_RANGE;
    }

    return std::clamp(int((std::clamp(elapsedMs, qint64(0), totalMs) *
                           RAID_STATUS_PROGRESS_RANGE) /
                          totalMs),
                      0, RAID_STATUS_PROGRESS_RANGE);
}

QString warningMessageError(HelixWarnUserError error, const QString &message,
                            const QString &displayName)
{
    using Error = HelixWarnUserError;

    switch (error)
    {
        case Error::ConflictingOperation:
            return QStringLiteral("There was a conflicting warn operation on "
                                  "this user. Please try again.");

        case Error::Forwarded:
            return message;

        case Error::Ratelimited:
            return QStringLiteral("You are being ratelimited by Twitch. Try "
                                  "again in a few seconds.");

        case Error::CannotWarnUser:
            return QStringLiteral("You cannot warn %1.").arg(displayName);

        case Error::UserMissingScope:
            return QStringLiteral("Missing required scope. Re-login with your "
                                  "account and try again.");

        case Error::UserNotAuthorized:
            return QStringLiteral(
                "You don't have permission to perform that action.");

        case Error::Unknown:
            return QStringLiteral("An unknown error has occurred.");
    }

    return QStringLiteral("An unknown error has occurred.");
}

void sendWarningMessageToTarget(const ChannelPtr &channel,
                                const QString &broadcasterID,
                                const QString &moderatorID,
                                const QString &reason)
{
    static QString targetUserID;
    static QString targetDisplayName;

    auto warnTarget = [channel, broadcasterID, moderatorID, reason](
                          const QString &targetID, const QString &displayName) {
        getHelix()->warnUser(
            broadcasterID, moderatorID, targetID, reason,
            [] {
                // Twitch emits the visible moderation line separately.
            },
            [channel, displayName](auto error, auto message) {
                if (channel == nullptr)
                {
                    return;
                }

                channel->addSystemMessage(
                    QStringLiteral("Failed to send message as warning - %1")
                        .arg(warningMessageError(error, message, displayName)));
            });
    };

    if (!targetUserID.isEmpty())
    {
        warnTarget(targetUserID, targetDisplayName);
        return;
    }

    getHelix()->fetchUsers(
        {}, {QString::fromLatin1(WARNING_MESSAGE_TARGET_LOGIN)},
        [channel, warnTarget](const auto &users) {
            auto it = std::find_if(
                users.begin(), users.end(), [](const HelixUser &user) {
                    return user.login.compare(QString::fromLatin1(
                                                  WARNING_MESSAGE_TARGET_LOGIN),
                                              Qt::CaseInsensitive) == 0;
                });

            if (it == users.end())
            {
                if (channel != nullptr)
                {
                    channel->addSystemMessage(
                        QStringLiteral("Failed to send message as warning - "
                                       "bad target username: %1")
                            .arg(QString::fromLatin1(
                                WARNING_MESSAGE_TARGET_LOGIN)));
                }
                return;
            }

            targetUserID = it->id;
            targetDisplayName =
                it->displayName.isEmpty() ? it->login : it->displayName;
            warnTarget(targetUserID, targetDisplayName);
        },
        [channel] {
            if (channel != nullptr)
            {
                channel->addSystemMessage(
                    QStringLiteral("Failed to send message as warning - "
                                   "could not look up %1.")
                        .arg(
                            QString::fromLatin1(WARNING_MESSAGE_TARGET_LOGIN)));
            }
        });
}

}  // namespace

SplitInput::SplitInput(Split *_chatWidget, bool enableInlineReplying)
    : SplitInput(_chatWidget, _chatWidget, _chatWidget->view_,
                 enableInlineReplying)
{
}

SplitInput::SplitInput(QWidget *parent, Split *_chatWidget,
                       ChannelView *_channelView, bool enableInlineReplying)
    : BaseWidget(parent)
    , split_(_chatWidget)
    , channelView_(_channelView)
    , enableInlineReplying_(enableInlineReplying)
    , raidStatusProgressAnimation_(this)
    , backgroundColorAnimation(this, "backgroundColor"_ba)
{
    this->installEventFilter(this);
    this->initLayout();
    this->raidStatusProgressAnimation_.setTargetObject(
        this->ui_.raidStatusProgress);
    this->raidStatusProgressAnimation_.setPropertyName("value"_ba);
    this->raidStatusProgressAnimation_.setEasingCurve(QEasingCurve::Linear);
    this->raidStatusProgressAnimation_.setDuration(
        RAID_STATUS_PROGRESS_TICK_MS);
    this->nukePreviewTimer_.setSingleShot(true);
    this->nukePreviewTimer_.setInterval(80);
    QObject::connect(&this->nukePreviewTimer_, &QTimer::timeout, this,
                     &SplitInput::applyNukePreview);
    this->outgoingTranslationPreviewTimer_.setSingleShot(true);
    this->outgoingTranslationPreviewTimer_.setInterval(
        OUTGOING_TRANSLATION_DEBOUNCE_MS);
    QObject::connect(&this->outgoingTranslationPreviewTimer_, &QTimer::timeout,
                     this, &SplitInput::applyOutgoingTranslationPreview);
    this->raidStatusTimer_.setSingleShot(false);
    this->raidStatusTimer_.setInterval(RAID_STATUS_PROGRESS_TICK_MS);
    QObject::connect(&this->raidStatusTimer_, &QTimer::timeout, this,
                     &SplitInput::updateRaidStatus);

    // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer)
    auto *spellChecker = getApp()->getSpellChecker();
    this->inputHighlighter = new InputHighlighter(*spellChecker, this);
    this->updateChannel();

    this->signalHolder_.managedConnect(this->split_->channelChanged, [this] {
        this->updateChannel();
    });

    getSettings()->enablePredictions.connect(
        [this](const auto &, auto) {
            this->bindChannelPoints(dynamic_cast<TwitchChannel *>(
                this->split_->getSelectedChannel().get()));
        },
        this->managedConnections_);
    getSettings()->showPredictionButton.connect(
        [this](const auto &, auto) {
            this->bindChannelPoints(dynamic_cast<TwitchChannel *>(
                this->split_->getSelectedChannel().get()));
        },
        this->managedConnections_);
    getSettings()->enablePolls.connect(
        [this](const auto &, auto) {
            this->bindChannelPoints(dynamic_cast<TwitchChannel *>(
                this->split_->getSelectedChannel().get()));
        },
        this->managedConnections_);
    getSettings()->showPollButton.connect(
        [this](const auto &, auto) {
            this->bindChannelPoints(dynamic_cast<TwitchChannel *>(
                this->split_->getSelectedChannel().get()));
        },
        this->managedConnections_);
    getSettings()->enableChannelPointsDisplay.connect(
        [this](const auto &, auto) {
            this->bindChannelPoints(dynamic_cast<TwitchChannel *>(
                this->split_->getSelectedChannel().get()));
        },
        this->managedConnections_);
    getSettings()->openRewardsWithChannelPointsClick.connect(
        [this](const auto &, auto) {
            this->bindChannelPoints(dynamic_cast<TwitchChannel *>(
                this->split_->getSelectedChannel().get()));
        },
        this->managedConnections_);
    getSettings()->customPinAuthToken.connect(
        [this](const QString &token, auto) {
            if (token.trimmed().isEmpty())
            {
                return;
            }

            if (dynamic_cast<TwitchChannel *>(
                    this->split_->getSelectedChannel().get()) != nullptr)
            {
                this->split_->scheduleDeferredTwitchRefresh(true);
            }
        },
        this->managedConnections_);
    getSettings()->moltorinoAuthAccounts.connect(
        [this](const QString &, auto) {
            if (dynamic_cast<TwitchChannel *>(
                    this->split_->getSelectedChannel().get()) != nullptr)
            {
                this->split_->scheduleDeferredTwitchRefresh(true);
            }
        },
        this->managedConnections_);
    getSettings()->hideEmojiButton.connect(
        [this](const auto &, auto) {
            this->updateEmoteButton();
        },
        this->managedConnections_);
    getSettings()->nukePreviewEnabled.connect(
        [this](const auto &, auto) {
            this->updateNukePreview(this->ui_.textEdit->toPlainText());
        },
        this->managedConnections_);
    getSettings()->showRaidStatusAboveInput.connect(
        [this](const auto &, auto) {
            this->updateRaidStatus();
        },
        this->managedConnections_);
    getSettings()->showCommandSuggestions.connect(
        [this](const bool enabled, auto) {
            if (enabled)
            {
                this->updateCompletionPopup();
            }
            else
            {
                this->resetCommandCompletionSession();
            }
        },
        this->managedConnections_);
    getSettings()->showOutgoingTranslationButton.connect(
        [this](const bool &, auto) {
            this->updateOutgoingTranslationButton();
            this->updateActionRowCompactness();
        },
        this->managedConnections_);
    getSettings()->outgoingTranslationMode.connect(
        [this](const QString &, auto) {
            this->updateOutgoingTranslationButton();
            this->updateOutgoingTranslationPreview();
        },
        this->managedConnections_);
    getSettings()->outgoingTranslationTargetLanguage.connect(
        [this](const QString &, auto) {
            this->outgoingTranslationPreviewSource_.clear();
            this->updateOutgoingTranslationButton();
            this->updateOutgoingTranslationPreview();
        },
        this->managedConnections_);

    getSettings()->enableSpellChecking.connect(
        [this] {
            this->checkSpellingChanged();
        },
        this->signalHolder_);

    // misc
    this->installTextEditEvents();
    this->addShortcuts();
    // The textEdit's signal will be destroyed before this SplitInput is
    // destroyed, so we can safely ignore this signal's connection.
    std::ignore = this->ui_.textEdit->focusLost.connect([this] {
        this->hideCompletionPopup();
        this->resetCommandCompletionSession();
    });
    this->scaleChangedEvent(this->scale());
    this->signalHolder_.managedConnect(getApp()->getHotkeys()->onItemsUpdated,
                                       [this]() {
                                           this->clearShortcuts();
                                           this->addShortcuts();
                                       });

    QEasingCurve curve;
    curve.setCustomType(highlightEasingFunction);
    this->backgroundColorAnimation.setDuration(500);
    this->backgroundColorAnimation.setEasingCurve(curve);
}

void SplitInput::initLayout()
{
    auto *app = getApp();
    LayoutCreator<SplitInput> layoutCreator(this);

    auto layout =
        layoutCreator.setLayoutType<QVBoxLayout>().withoutMargin().assign(
            &this->ui_.vbox);
    layout->setSpacing(0);
    this->applyOuterMargin();

    // reply label stuff
    auto replyWrapper =
        layout.emplace<QWidget>().assign(&this->ui_.replyWrapper);
    replyWrapper->setContentsMargins(0, 0, 1, 1);

    auto replyVbox =
        replyWrapper.setLayoutType<QVBoxLayout>().withoutMargin().assign(
            &this->ui_.replyVbox);
    replyVbox->setSpacing(1);

    auto replyHbox =
        replyVbox.emplace<QHBoxLayout>().assign(&this->ui_.replyHbox);

    auto *messageVbox = new QVBoxLayout;
    this->ui_.replyMessage = new MessageView();
    messageVbox->addWidget(this->ui_.replyMessage, 0, Qt::AlignLeft);
    messageVbox->setContentsMargins(10, 0, 0, 0);
    replyVbox->addLayout(messageVbox, 0);

    auto replyLabel = replyHbox.emplace<QLabel>().assign(&this->ui_.replyLabel);
    replyLabel->setAlignment(Qt::AlignLeft);
    replyLabel->setFont(
        app->getFonts()->getFont(FontStyle::ChatMedium, this->scale()));

    replyHbox->addStretch(1);

    auto replyCancelButton = replyHbox
                                 .emplace<SvgButton>(
                                     SvgButton::Src{
                                         .dark = ":/buttons/cancel.svg",
                                         .light = ":/buttons/cancelDark.svg",
                                     },
                                     nullptr, QSize{4, 0})
                                 .assign(&this->ui_.cancelReplyButton);

    replyCancelButton->hide();
    replyLabel->hide();

    auto raidStatusWidget =
        layout.emplace<QWidget>().assign(&this->ui_.raidStatusWidget);
    raidStatusWidget->setObjectName("raidStatusWidget");
    raidStatusWidget->setAttribute(Qt::WA_StyledBackground, true);
    raidStatusWidget->setContentsMargins(0, 0, 0, 0);
    auto raidStatusLayout =
        raidStatusWidget.setLayoutType<QVBoxLayout>().withoutMargin().assign(
            &this->ui_.raidStatusLayout);
    raidStatusLayout->setSpacing(0);
    raidStatusLayout.emplace<QLabel>().assign(&this->ui_.raidStatusLabel);
    this->ui_.raidStatusLabel->setObjectName("raidStatusLabel");
    this->ui_.raidStatusLabel->setContentsMargins(6, 1, 6, 3);
    this->ui_.raidStatusLabel->setTextFormat(Qt::PlainText);
    this->ui_.raidStatusLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto raidProgress = raidStatusLayout.emplace<QProgressBar>().assign(
        &this->ui_.raidStatusProgress);
    raidProgress->setObjectName("raidStatusProgress");
    raidProgress->setRange(0, RAID_STATUS_PROGRESS_RANGE);
    raidProgress->setTextVisible(false);
    raidProgress->setFixedHeight(std::max(2, int(2 * this->scale())));
    raidStatusWidget->hide();

    auto nukePreviewLabel =
        layout.emplace<QLabel>().assign(&this->ui_.nukePreviewLabel);
    nukePreviewLabel->setContentsMargins(6, 1, 6, 1);
    nukePreviewLabel->setStyleSheet(
        "color: #ffb3b3; background: rgba(90, 20, 20, 0.32);");
    nukePreviewLabel->hide();

    auto commandCompletionWidget =
        layout.emplace<QWidget>().assign(&this->ui_.commandCompletionWidget);
    commandCompletionWidget->setObjectName("commandCompletionWidget");
    commandCompletionWidget->setAttribute(Qt::WA_StyledBackground, true);
    commandCompletionWidget->setContentsMargins(0, 0, 0, 0);
    commandCompletionWidget->installEventFilter(this);
    auto commandCompletionLayout =
        commandCompletionWidget.setLayoutType<QVBoxLayout>()
            .withoutMargin()
            .assign(&this->ui_.commandCompletionLayout);
    commandCompletionLayout->setSpacing(0);
    for (size_t i = 0; i < this->ui_.commandCompletionRows.size(); ++i)
    {
        auto row = commandCompletionLayout.emplace<QLabel>().assign(
            &this->ui_.commandCompletionRows[i]);
        row->setObjectName("commandCompletionRow");
        row->setTextFormat(Qt::RichText);
        row->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        row->setContentsMargins(6, 1, 6, 1);
        row->setCursor(Qt::PointingHandCursor);
        row->setProperty("commandCompletionRow", int(i));
        row->installEventFilter(this);
    }
    commandCompletionWidget->hide();

    auto translationPreviewWidget =
        layout.emplace<QWidget>().assign(&this->ui_.translationPreviewWidget);
    translationPreviewWidget->setObjectName("translationPreviewWidget");
    translationPreviewWidget->setAttribute(Qt::WA_StyledBackground, true);
    translationPreviewWidget->setContentsMargins(0, 0, 0, 0);
    auto translationPreviewLayout =
        translationPreviewWidget.setLayoutType<QHBoxLayout>().withoutMargin();
    translationPreviewLayout->setContentsMargins(6, 1, 6, 1);
    translationPreviewLayout.emplace<QLabel>().assign(
        &this->ui_.translationPreviewLabel);
    this->ui_.translationPreviewLabel->setObjectName("translationPreviewLabel");
    this->ui_.translationPreviewLabel->setTextFormat(Qt::PlainText);
    this->ui_.translationPreviewLabel->setAlignment(Qt::AlignLeft |
                                                    Qt::AlignVCenter);
    this->ui_.translationPreviewLabel->setWordWrap(false);
    this->ui_.translationPreviewLabel->setMinimumWidth(0);
    this->ui_.translationPreviewLabel->setSizePolicy(QSizePolicy::Ignored,
                                                     QSizePolicy::Fixed);
    translationPreviewWidget->hide();

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
    auto rewardPromptWidget = layout.emplace<QWidget>().assign(
        &this->ui_.channelPointRewardPromptWidget);
    rewardPromptWidget->setObjectName("channelPointRewardPromptWidget");
    rewardPromptWidget->setAttribute(Qt::WA_StyledBackground, true);
    auto rewardPromptLayout =
        rewardPromptWidget.setLayoutType<QHBoxLayout>().withoutMargin();
    rewardPromptLayout->setContentsMargins(6, 1, 6, 1);
    rewardPromptLayout.emplace<QLabel>().assign(
        &this->ui_.channelPointRewardPromptTitle);
    this->ui_.channelPointRewardPromptTitle->setObjectName(
        "channelPointRewardPromptTitle");
    this->ui_.channelPointRewardPromptTitle->setTextFormat(Qt::PlainText);
    this->ui_.channelPointRewardPromptTitle->setAlignment(Qt::AlignLeft |
                                                          Qt::AlignVCenter);
    rewardPromptWidget->hide();
#endif

    auto inputWrapper =
        layout.emplace<QWidget>().assign(&this->ui_.inputWrapper);
    inputWrapper->setContentsMargins(1, 1, 1, 1);

    auto hboxLayout =
        inputWrapper.setLayoutType<QHBoxLayout>().withoutMargin().assign(
            &this->ui_.inputHbox);

    auto textEdit =
        hboxLayout.emplace<ResizingTextEdit>().assign(&this->ui_.textEdit);
    connect(textEdit.getElement(), &ResizingTextEdit::textChanged, this,
            &SplitInput::editTextChanged);
    textEdit->setFrameStyle(QFrame::NoFrame);
    this->ui_.inputHbox->setStretch(0, 1);

    auto *shortcutFilter = new CmdDeleteKeyFilter(this);
    textEdit->installEventFilter(shortcutFilter);
    textEdit->installEventFilter(this);
    textEdit->viewport()->installEventFilter(this);

    hboxLayout.emplace<LabelButton>("SEND").assign(&this->ui_.sendButton);
    this->ui_.sendButton->hide();

    QObject::connect(this->ui_.sendButton, &Button::leftClicked, [this] {
        std::vector<QString> arguments;
        this->handleSendMessage(arguments);
    });

    getSettings()->showSendButton.connect(
        [this](const bool value, auto) {
            if (value)
            {
                this->ui_.sendButton->show();
            }
            else
            {
                this->ui_.sendButton->hide();
            }
        },
        this->managedConnections_);

    auto box = hboxLayout.emplace<QVBoxLayout>().withoutMargin();
    box->setSpacing(0);
    {
        this->ui_.textEditLength = new QLabel();
        this->ui_.textEditLength->setAlignment(Qt::AlignRight);
        makeInputActionLabelCompressible(this->ui_.textEditLength);
        this->ui_.textEditLength->hide();

        this->ui_.sendWaitStatus = new QLabel();
        this->ui_.sendWaitStatus->setAlignment(Qt::AlignRight |
                                               Qt::AlignBottom);
        makeInputActionLabelCompressible(this->ui_.sendWaitStatus);
        this->ui_.sendWaitStatus->setHidden(true);

        this->ui_.channelPointsLabel = new QLabel();
        this->ui_.channelPointsLabel->setAlignment(Qt::AlignRight |
                                                   Qt::AlignBottom);
        makeInputActionLabelSticky(this->ui_.channelPointsLabel);
        this->ui_.channelPointsLabel->setToolTip(channelPointsIdleToolTip());
        this->ui_.channelPointsLabel->setCursor(Qt::PointingHandCursor);
        this->ui_.channelPointsLabel->installEventFilter(this);
        this->ui_.channelPointsLabel->hide();

        this->ui_.predictionButton = new SvgButton(
            {
                .dark = ":/buttons/prediction-darkMode.svg",
                .light = ":/buttons/prediction-lightMode.svg",
            },
            nullptr, QSize{3, 3});
        this->ui_.predictionButton->setToolTip("Create prediction");
        this->ui_.predictionButton->hide();

        this->ui_.pollButton = new SvgButton(
            {
                .dark = ":/buttons/poll-darkMode.svg",
                .light = ":/buttons/poll-lightMode.svg",
            },
            nullptr, QSize{3, 3});
        this->ui_.pollButton->setToolTip("Open poll");
        this->ui_.pollButton->hide();

        this->ui_.outgoingTranslateButton = new SvgButton(
            {
                .dark = ":/buttons/translate-darkMode.svg",
                .light = ":/buttons/translate-lightMode.svg",
            },
            nullptr, QSize{2, 1});
        this->ui_.outgoingTranslateButton->setToolTip("Outgoing translation");
        this->ui_.outgoingTranslateButton->hide();

        this->ui_.emoteButton = new SvgButton(
            {
                .dark = ":/buttons/emote.svg",
                .light = ":/buttons/emoteDark.svg",
            },
            nullptr, QSize{3, 3});

        box->addStretch(1);
        box->addWidget(this->ui_.textEditLength, 0, Qt::AlignRight);

        auto *buttonsRow = new QHBoxLayout();
        this->ui_.buttonsRow = buttonsRow;
        buttonsRow->setContentsMargins(0, 0, 0, 0);
        buttonsRow->setSpacing(0);
        buttonsRow->addWidget(this->ui_.sendWaitStatus, 0, Qt::AlignBottom);
        buttonsRow->addWidget(this->ui_.channelPointsLabel, 0, Qt::AlignBottom);
        buttonsRow->addWidget(this->ui_.predictionButton, 0, Qt::AlignBottom);
        buttonsRow->addWidget(this->ui_.pollButton, 0, Qt::AlignBottom);
        buttonsRow->addWidget(this->ui_.outgoingTranslateButton, 0,
                              Qt::AlignBottom);
        buttonsRow->addWidget(this->ui_.emoteButton, 0, Qt::AlignBottom);
        box->addLayout(buttonsRow);
    }

    // ---- misc

    // set edit font
    this->ui_.textEdit->setFont(
        app->getFonts()->getFont(FontStyle::ChatMedium, this->scale()));
    QObject::connect(this->ui_.textEdit, &QTextEdit::cursorPositionChanged,
                     this, &SplitInput::onCursorPositionChanged);
    QObject::connect(this->ui_.textEdit, &QTextEdit::textChanged, this,
                     &SplitInput::onTextChanged);

    this->managedConnections_.managedConnect(app->getFonts()->fontChanged,
                                             [this] {
                                                 this->updateFonts();
                                             });

    // open emote popup
    QObject::connect(this->ui_.emoteButton, &Button::leftClicked, [this] {
        this->openEmotePopup();
    });
    QObject::connect(this->ui_.outgoingTranslateButton, &Button::leftClicked,
                     [this] {
                         this->openOutgoingTranslationMenu();
                     });

    // open prediction dialog (mod/broadcaster only)
    QObject::connect(this->ui_.predictionButton, &Button::leftClicked, [this] {
        auto *channel = dynamic_cast<TwitchChannel *>(
            this->split_->getSelectedChannel().get());
        if (!channel)
        {
            return;
        }
        PredictionDialog::showDialog(channel, this->split_);
    });
    QObject::connect(this->ui_.pollButton, &Button::leftClicked, [this] {
        auto *channel = dynamic_cast<TwitchChannel *>(
            this->split_->getSelectedChannel().get());
        if (!channel)
        {
            return;
        }
        PollDialog::showDialog(channel, this->split_);
    });

    // clear input and remove reply thread
    QObject::connect(this->ui_.cancelReplyButton, &Button::leftClicked, [this] {
        this->setReply(nullptr, {});
    });

    // Forward selection change signal
    QObject::connect(this->ui_.textEdit, &QTextEdit::copyAvailable,
                     [this](bool available) {
                         if (available)
                         {
                             this->selectionChanged.invoke();
                         }
                     });

    // textEditLength visibility
    getSettings()->showMessageLength.connect(
        [this](const bool &value, auto) {
            // this->ui_.textEditLength->setHidden(!value);
            this->editTextChanged();
        },
        this->managedConnections_);

    // sendWaitStatus visibility
    getSettings()->showSendWaitTimer.connect(
        [this](bool value, const auto &) {
            this->sendWaitStatusWanted_ =
                value && !this->ui_.sendWaitStatus->text().isEmpty();
            this->updateActionRowCompactness();
        },
        this->managedConnections_);
}

void SplitInput::triggerSelfMessageReceived()
{
    if (this->backgroundColorAnimation.state() != QPropertyAnimation::Stopped)
    {
        this->backgroundColorAnimation.stop();
    }
    this->backgroundColorAnimation.setDirection(QPropertyAnimation::Forward);
    this->backgroundColorAnimation.start();
}

QSize SplitInput::minimumSizeHint() const
{
    auto hint = QWidget::minimumSizeHint();
    if (this->ui_.textEdit == nullptr || this->ui_.inputWrapper == nullptr)
    {
        return hint;
    }

    const auto wrapperMargins = this->ui_.inputWrapper->contentsMargins();
    const auto outerMargins = this->ui_.vbox != nullptr
                                  ? this->ui_.vbox->contentsMargins()
                                  : QMargins{};
    const int textWidth = std::max(36, int(std::round(44 * this->scale())));
    const int emoteWidth = getSettings()->hideEmojiButton
                               ? 0
                               : compactWidgetWidth(this->ui_.emoteButton);
    const int translateWidth =
        getSettings()->showOutgoingTranslationButton
            ? compactWidgetWidth(this->ui_.outgoingTranslateButton)
            : 0;
    const int sendWidth =
        this->ui_.sendButton != nullptr && this->ui_.sendButton->isVisible()
            ? compactWidgetWidth(this->ui_.sendButton)
            : 0;
    const int compactWidth = textWidth + emoteWidth + translateWidth +
                             sendWidth + wrapperMargins.left() +
                             wrapperMargins.right() + outerMargins.left() +
                             outerMargins.right() + 2;

    hint.setWidth(compactWidth);
    return hint;
}

void SplitInput::scaleChangedEvent(float scale)
{
    // update the icon size of the buttons
    this->updateEmoteButton();
    this->updateCancelReplyButton();

    // set maximum height
    if (!this->hidden)
    {
        this->setMaximumHeight(this->scaledMaxHeight());
        if (this->replyTarget_ != nullptr)
        {
            this->ui_.vbox->setSpacing(this->marginForTheme());
        }
    }
    this->applyOuterMargin();
    this->updateFonts();
    this->relayoutParentWidgets();
}

void SplitInput::relayoutParentWidgets()
{
    if (auto *layout = this->layout())
    {
        layout->invalidate();
        layout->activate();
    }

    this->updateGeometry();

    if (auto *parent = this->parentWidget())
    {
        if (auto *lay = parent->layout())
        {
            lay->invalidate();
        }
        parent->updateGeometry();
    }
}

void SplitInput::themeChangedEvent()
{
    QPalette palette;

    palette.setColor(QPalette::WindowText, this->theme->splits.input.text);

    this->ui_.textEditLength->setPalette(palette);
    this->ui_.sendWaitStatus->setPalette(palette);

    QPalette pointsPalette = palette;
    auto channelPointsColor = this->theme->splits.input.text;
    channelPointsColor.setAlphaF(0.72F);
    pointsPalette.setColor(QPalette::WindowText, channelPointsColor);
    this->ui_.channelPointsLabel->setPalette(pointsPalette);
    this->ui_.sendWaitStatus->setPalette(pointsPalette);

    // Theme changed, reset current background color
    this->setBackgroundColor(this->theme->splits.input.background);
    this->backgroundColorAnimation.setStartValue(
        this->theme->splits.input.backgroundPulse);
    this->backgroundColorAnimation.setEndValue(
        this->theme->splits.input.background);
    this->backgroundColorAnimation.stop();
    this->updateTextEditPalette();

    if (this->theme->isLightTheme())
    {
        this->ui_.replyLabel->setStyleSheet("color: #333");
        this->ui_.raidStatusWidget->setStyleSheet(
            "QWidget#raidStatusWidget { background-color: rgba(105, 70, "
            "180, 0.18); border: 0; }");
        this->ui_.raidStatusLabel->setStyleSheet(
            "QLabel#raidStatusLabel { color: #4d237d; background: "
            "transparent; }");
        this->ui_.raidStatusProgress->setStyleSheet(
            "QProgressBar#raidStatusProgress { border: 0; background: "
            "transparent; margin: 0; padding: 0; }"
            "QProgressBar#raidStatusProgress::chunk { background-color: "
            "#7c3aed; border: 0; }");
        this->ui_.nukePreviewLabel->setStyleSheet(
            "color: #7a1111; background: rgba(255, 70, 70, 0.20);");
        if (this->ui_.commandCompletionWidget != nullptr)
        {
            this->ui_.commandCompletionWidget->setStyleSheet(
                "QWidget#commandCompletionWidget { background: rgba(245, "
                "245, 245, 0.92); border-top: 1px solid rgba(0, 0, 0, "
                "0.16); }");
        }
        if (this->ui_.translationPreviewWidget != nullptr)
        {
            this->ui_.translationPreviewWidget->setStyleSheet(
                "QWidget#translationPreviewWidget { background: rgba(255, "
                "138, 0, 0.13); border-top: 1px solid rgba(255, 138, 0, "
                "0.35); }"
                "QLabel#translationPreviewLabel { color: #6b3300; "
                "background: transparent; }");
        }
#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
        this->ui_.channelPointRewardPromptWidget->setStyleSheet(
            "QWidget#channelPointRewardPromptWidget { background: rgba(145, "
            "70, 255, 0.14); border-top: 1px solid rgba(145, 70, 255, "
            "0.45); }"
            "QLabel#channelPointRewardPromptTitle { color: #4d237d; "
            "background: transparent; }");
#endif
    }
    else
    {
        this->ui_.replyLabel->setStyleSheet("color: #ccc");
        this->ui_.raidStatusWidget->setStyleSheet(
            "QWidget#raidStatusWidget { background-color: rgba(91, 46, "
            "153, 0.82); border: 0; }");
        this->ui_.raidStatusLabel->setStyleSheet(
            "QLabel#raidStatusLabel { color: #f1eaff; background: "
            "transparent; }");
        this->ui_.raidStatusProgress->setStyleSheet(
            "QProgressBar#raidStatusProgress { border: 0; background: "
            "transparent; margin: 0; padding: 0; }"
            "QProgressBar#raidStatusProgress::chunk { background-color: "
            "#c4b5fd; border: 0; }");
        this->ui_.nukePreviewLabel->setStyleSheet(
            "color: #ffb3b3; background: rgba(90, 20, 20, 0.32);");
        if (this->ui_.commandCompletionWidget != nullptr)
        {
            this->ui_.commandCompletionWidget->setStyleSheet(
                "QWidget#commandCompletionWidget { background: rgba(18, 18, "
                "22, 0.94); border-top: 1px solid rgba(255, 255, 255, "
                "0.14); }");
        }
        if (this->ui_.translationPreviewWidget != nullptr)
        {
            this->ui_.translationPreviewWidget->setStyleSheet(
                "QWidget#translationPreviewWidget { background: rgba(120, "
                "68, 0, 0.78); border-top: 1px solid #ff8a00; }"
                "QLabel#translationPreviewLabel { color: #ffe7c2; "
                "background: transparent; }");
        }
#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
        this->ui_.channelPointRewardPromptWidget->setStyleSheet(
            "QWidget#channelPointRewardPromptWidget { background: rgba(91, "
            "46, 153, 0.82); border-top: 1px solid #b982ff; }"
            "QLabel#channelPointRewardPromptTitle { color: #f1eaff; "
            "background: transparent; }");
#endif
    }

    // update vbox
    this->applyOuterMargin();
    if (this->replyTarget_ != nullptr)
    {
        this->ui_.vbox->setSpacing(this->marginForTheme());
    }
    if (this->ui_.commandCompletionWidget != nullptr)
    {
        this->renderCommandCompletion();
    }
}

void SplitInput::updateEmoteButton()
{
    auto scale = this->scale();
    auto buttonSize = int(std::round(17 * scale));
    auto labelBottomInset = int(std::round(1 * scale));

    this->ui_.emoteButton->setFixedSize(buttonSize, buttonSize);

    if (this->ui_.buttonsRow)
    {
        this->ui_.buttonsRow->setSpacing(0);
        this->ui_.buttonsRow->setContentsMargins(0, 0,
                                                 int(std::round(2 * scale)), 0);
    }

    if (this->ui_.sendWaitStatus)
    {
        this->ui_.sendWaitStatus->setContentsMargins(
            0, 0, int(std::round(3 * scale)), labelBottomInset);
    }
    if (this->ui_.textEditLength)
    {
        this->ui_.textEditLength->setContentsMargins(
            0, 0, int(std::round(2 * scale)), 0);
    }

    if (this->ui_.channelPointsLabel)
    {
        this->ui_.channelPointsLabel->setContentsMargins(
            0, 0, int(std::round(2 * scale)), labelBottomInset);
    }

    if (this->ui_.predictionButton)
    {
        this->ui_.predictionButton->setFixedSize(buttonSize, buttonSize);
    }
    if (this->ui_.pollButton)
    {
        this->ui_.pollButton->setFixedSize(buttonSize, buttonSize);
    }
    if (this->ui_.outgoingTranslateButton)
    {
        this->ui_.outgoingTranslateButton->setFixedSize(buttonSize, buttonSize);
    }

    this->updateActionRowCompactness();
}

void SplitInput::updateCancelReplyButton()
{
    float scale = this->scale();

    this->ui_.cancelReplyButton->setFixedHeight(int(12 * scale));
    this->ui_.cancelReplyButton->setFixedWidth(int(20 * scale));
}

void SplitInput::updateActionRowCompactness()
{
    if (this->ui_.buttonsRow == nullptr || this->ui_.inputWrapper == nullptr)
    {
        return;
    }

    updateInputActionLabelMinimumWidth(this->ui_.textEditLength);
    updateInputActionLabelMinimumWidth(this->ui_.sendWaitStatus);

    const auto inputWidth = this->ui_.inputWrapper->width();
    const bool hasRealWidth = inputWidth > 0;
    int budget =
        hasRealWidth
            ? inputWidth - std::max(56, int(std::round(92 * this->scale())))
            : std::numeric_limits<int>::max();

    auto tryFit = [&](QWidget *widget, bool wanted) {
        if (!wanted || widget == nullptr)
        {
            return false;
        }
        if (!hasRealWidth)
        {
            return true;
        }

        const auto width = compactWidgetWidth(widget);
        if (width > budget)
        {
            return false;
        }
        budget -= width;
        return true;
    };

    auto setExplicitVisible = [](QWidget *widget, bool visible) {
        if (widget == nullptr || widget->isHidden() == !visible)
        {
            return false;
        }
        widget->setVisible(visible);
        return true;
    };

    const bool showEmoteButton = !getSettings()->hideEmojiButton;
    const bool showTranslateButton =
        getSettings()->showOutgoingTranslationButton;
    if (hasRealWidth && this->ui_.sendButton != nullptr &&
        this->ui_.sendButton->isVisible())
    {
        budget -= compactWidgetWidth(this->ui_.sendButton);
    }
    if (hasRealWidth && showEmoteButton)
    {
        budget -= compactWidgetWidth(this->ui_.emoteButton);
    }
    if (hasRealWidth && showTranslateButton)
    {
        budget -= compactWidgetWidth(this->ui_.outgoingTranslateButton);
    }

    auto reserveEvenIfTight = [&](QWidget *widget, bool wanted) {
        if (!wanted || widget == nullptr)
        {
            return false;
        }
        if (hasRealWidth)
        {
            budget -= compactWidgetWidth(widget);
        }
        return true;
    };

    const bool showChannelPoints = reserveEvenIfTight(
        this->ui_.channelPointsLabel, this->channelPointsLabelWanted_);
    const bool showPredictionButton =
        tryFit(this->ui_.predictionButton, this->predictionButtonWanted_);
    const bool showPollButton =
        tryFit(this->ui_.pollButton, this->pollButtonWanted_);
    const bool showSendWaitStatus =
        tryFit(this->ui_.sendWaitStatus, this->sendWaitStatusWanted_);
    const bool showTextLength = !this->ui_.textEditLength->text().isEmpty();

    bool changed = false;
    changed |= setExplicitVisible(this->ui_.emoteButton, showEmoteButton);
    changed |= setExplicitVisible(this->ui_.outgoingTranslateButton,
                                  showTranslateButton);
    changed |=
        setExplicitVisible(this->ui_.predictionButton, showPredictionButton);
    changed |= setExplicitVisible(this->ui_.pollButton, showPollButton);
    changed |= setExplicitVisible(this->ui_.textEditLength, showTextLength);
    changed |= setExplicitVisible(this->ui_.sendWaitStatus, showSendWaitStatus);
    changed |=
        setExplicitVisible(this->ui_.channelPointsLabel, showChannelPoints);

    if (changed)
    {
        if (auto *layout = this->ui_.inputWrapper->layout())
        {
            layout->invalidate();
        }
        this->ui_.inputWrapper->updateGeometry();
        this->updateGeometry();
    }
}

void SplitInput::openEmotePopup()
{
    if (!this->emotePopup_)
    {
        this->emotePopup_ = new EmotePopup(this);
        this->emotePopup_->setAttribute(Qt::WA_DeleteOnClose);

        // The EmotePopup is closed & destroyed when this is destroyed, meaning it's safe to ignore this connection
        std::ignore =
            this->emotePopup_->linkClicked.connect([this](const Link &link) {
                if (link.type == Link::InsertText)
                {
                    QTextCursor cursor = this->ui_.textEdit->textCursor();
                    QString textToInsert(link.value + " ");

                    // If symbol before cursor isn't space or empty
                    // Then insert space before emote.
                    if (cursor.position() > 0 &&
                        !this->getInputText()[cursor.position() - 1].isSpace())
                    {
                        textToInsert = " " + textToInsert;
                    }
                    this->insertText(textToInsert);
                    this->ui_.textEdit->activateWindow();
                }
            });
    }

    this->emotePopup_->loadChannel(this->split_->getSelectedChannel());
    this->emotePopup_->show();
    this->emotePopup_->raise();
    this->emotePopup_->activateWindow();
}

QString SplitInput::handleSendMessage(const std::vector<QString> &arguments)
{
    ChannelPtr c;
    if (this->replyTarget_)
    {
        c = this->replyChannel_.lock();
    }
    if (!c)
    {
        c = this->split_->getSelectedChannel();
    }
    if (c == nullptr)
    {
        return "";
    }

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
    if (this->submitChannelPointRewardPrompt())
    {
        return "";
    }
#endif

    auto openPollDialogCommand = [this, &arguments,
                                  &c](const QString &message) {
        const auto trimmed = message.trimmed();
        if (trimmed.compare("/poll", Qt::CaseInsensitive) != 0 &&
            !trimmed.startsWith("/poll ", Qt::CaseInsensitive))
        {
            return false;
        }
        if (!getSettings()->enablePolls)
        {
            return false;
        }

        auto *channel = dynamic_cast<TwitchChannel *>(c.get());
        if (channel == nullptr)
        {
            c->addSystemMessage(
                "The /poll command only works in Twitch channels");
            this->postMessageSend(message, arguments);
            return true;
        }

        PollDialog::showDialog(channel, this->split_);
        this->postMessageSend(message, arguments);
        return true;
    };

    QString message = this->currentOutgoingMessageBody();
    if (openPollDialogCommand(message))
    {
        return "";
    }

    if (this->maybeSendTranslatedMessage(message, arguments, c))
    {
        return "";
    }

    if (this->maybeSendMessageAsWarning(message, arguments, c))
    {
        return "";
    }

    if (!c->isTwitchOrKickChannel() || this->replyTarget_ == nullptr)
    {
        QString sendMessage =
            getApp()->getCommands()->execCommand(message, c, false);
        c->sendMessage(sendMessage);

        this->postMessageSend(message, arguments);
        return "";
    }

    // Reply to message
    auto *tc = dynamic_cast<TwitchChannel *>(c.get());
    auto *kc = dynamic_cast<KickChannel *>(c.get());
    if (!tc && !kc)
    {
        // this should not fail
        return "";
    }

    QString sendMessage =
        getApp()->getCommands()->execCommand(message, c, false);

    // Reply within TwitchChannel
    if (tc)
    {
        tc->sendReply(sendMessage, this->replyTarget_->id);
    }
    else if (kc)
    {
        kc->sendReply(sendMessage, this->replyTarget_->id);
    }

    this->postMessageSend(message, arguments);
    return "";
}

void SplitInput::postMessageSend(const QString &message,
                                 const std::vector<QString> &arguments)
{
    // don't add duplicate messages and empty message to message history
    if ((this->prevMsg_.isEmpty() || !this->prevMsg_.endsWith(message)) &&
        !message.trimmed().isEmpty())
    {
        this->prevMsg_.append(message);
    }

    if (arguments.empty() || arguments.at(0) != "keepInput")
    {
        this->clearInput();
    }
    this->prevIndex_ = this->prevMsg_.size();
}

void SplitInput::postTranslatedMessageSend(
    const QString &message, const std::vector<QString> &arguments)
{
    if ((this->prevMsg_.isEmpty() || !this->prevMsg_.endsWith(message)) &&
        !message.trimmed().isEmpty())
    {
        this->prevMsg_.append(message);
    }

    if ((arguments.empty() || arguments.at(0) != "keepInput") &&
        this->currentOutgoingMessageBody().trimmed() == message.trimmed())
    {
        this->clearInput();
    }
    this->prevIndex_ = this->prevMsg_.size();
}

bool SplitInput::maybeSendTranslatedMessage(
    const QString &message, const std::vector<QString> &arguments,
    const ChannelPtr &channel)
{
    const auto mode =
        normalizedOutgoingTranslationMode(this->outgoingTranslationMode());
    if (mode != QLatin1String(OUTGOING_TRANSLATION_SEND) ||
        !this->shouldTranslateOutgoingMessage(message))
    {
        return false;
    }

    if (this->outgoingTranslationSendInFlight_)
    {
        if (this->ui_.translationPreviewWidget != nullptr)
        {
            this->ui_.translationPreviewLabel->setText(
                QStringLiteral("Translating before send..."));
            this->ui_.translationPreviewWidget->show();
        }
        return true;
    }

    if (channel == nullptr)
    {
        return false;
    }

    const auto targetLanguage = normalizedTranslationTargetLanguage(
        this->outgoingTranslationTargetLanguage());
    const auto targetName = translationLanguageName(targetLanguage);
    const auto sourceMessage = message.trimmed();
    const auto replyMessageID =
        this->replyTarget_ == nullptr ? QString{} : this->replyTarget_->id;
    this->outgoingTranslationSendInFlight_ = true;

    if (this->ui_.translationPreviewWidget != nullptr)
    {
        this->ui_.translationPreviewLabel->setText(
            QStringLiteral("Translating to %1 before send...").arg(targetName));
        this->ui_.translationPreviewWidget->show();
    }

    auto sendTranslated = [this, channel, sourceMessage, replyMessageID,
                           arguments](QString translatedText) {
        translatedText = translatedText.trimmed();
        translatedText.replace('\n', ' ');
        if (translatedText.isEmpty())
        {
            channel->addSystemMessage(
                QStringLiteral("Translation failed, so nothing was sent."));
            return;
        }
        if (translatedText.size() > TWITCH_MESSAGE_LIMIT)
        {
            channel->addSystemMessage(QStringLiteral(
                "The translated message is too long for Twitch."));
            return;
        }

        if (this->trySendMessageAsWarning(translatedText, channel))
        {
            this->postTranslatedMessageSend(sourceMessage, arguments);
            return;
        }

        if (!replyMessageID.isEmpty())
        {
            if (auto *tc = dynamic_cast<TwitchChannel *>(channel.get()))
            {
                tc->sendReply(translatedText, replyMessageID);
            }
            else if (auto *kc = dynamic_cast<KickChannel *>(channel.get()))
            {
                kc->sendReply(translatedText, replyMessageID);
            }
            else
            {
                channel->sendMessage(translatedText);
            }
        }
        else
        {
            channel->sendMessage(translatedText);
        }

        this->postTranslatedMessageSend(sourceMessage, arguments);
    };

    if (this->outgoingTranslationPreviewSource_ == sourceMessage &&
        this->outgoingTranslationPreviewTarget_ == targetLanguage &&
        !this->outgoingTranslationPreviewText_.isEmpty())
    {
        sendTranslated(this->outgoingTranslationPreviewText_);
        this->outgoingTranslationSendInFlight_ = false;
        return true;
    }

    requestTextTranslation(
        sourceMessage, targetLanguage, this,
        [this, sendTranslated = std::move(sendTranslated)](
            const TranslationResult &result) mutable {
            sendTranslated(result.translatedText);
        },
        [channel](const QString &) {
            channel->addSystemMessage(
                QStringLiteral("Translation failed, so nothing was sent."));
        },
        [this] {
            this->outgoingTranslationSendInFlight_ = false;
        });

    return true;
}

bool SplitInput::trySendMessageAsWarning(const QString &message,
                                         const ChannelPtr &channel)
{
    if (!getSettings()->sendMessageAsWarnings)
    {
        return false;
    }

    const auto trimmed = message.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith('/'))
    {
        return false;
    }

    auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get());
    if (twitchChannel == nullptr ||
        (!twitchChannel->isMod() && !twitchChannel->isBroadcaster()))
    {
        return false;
    }

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser->isAnon())
    {
        channel->addSystemMessage(
            QStringLiteral("You must be logged in to send warnings."));
        return true;
    }

    const auto broadcasterID = twitchChannel->roomId();
    if (broadcasterID.isEmpty())
    {
        channel->addSystemMessage(
            QStringLiteral("Cannot send warning: channel ID not available."));
        return true;
    }

    sendWarningMessageToTarget(channel, broadcasterID, currentUser->getUserId(),
                               trimmed);
    return true;
}

bool SplitInput::maybeSendMessageAsWarning(
    const QString &message, const std::vector<QString> &arguments,
    const ChannelPtr &channel)
{
    if (!this->trySendMessageAsWarning(message, channel))
    {
        return false;
    }

    this->postMessageSend(message, arguments);
    return true;
}

int SplitInput::scaledMaxHeight() const
{
    if (this->replyTarget_ != nullptr)
    {
        // give more space for showing the message being replied to
        return int(250 * this->scale());
    }
    else
    {
        return int(150 * this->scale());
    }
}

void SplitInput::addShortcuts()
{
    HotkeyController::HotkeyMap actions{
        {"cursorToStart",
         [this](const std::vector<QString> &arguments) -> QString {
             if (arguments.size() != 1)
             {
                 qCWarning(chatterinoHotkeys)
                     << "Invalid cursorToStart arguments. Argument 0: select "
                        "(\"withSelection\" or \"withoutSelection\")";
                 return "Invalid cursorToStart arguments. Argument 0: select "
                        "(\"withSelection\" or \"withoutSelection\")";
             }
             QTextCursor cursor = this->ui_.textEdit->textCursor();
             auto place = QTextCursor::Start;
             const auto &stringTakeSelection = arguments.at(0);
             bool select{};
             if (stringTakeSelection == "withSelection")
             {
                 select = true;
             }
             else if (stringTakeSelection == "withoutSelection")
             {
                 select = false;
             }
             else
             {
                 qCWarning(chatterinoHotkeys)
                     << "Invalid cursorToStart select argument (0)!";
                 return "Invalid cursorToStart select argument (0)!";
             }

             cursor.movePosition(place,
                                 select ? QTextCursor::MoveMode::KeepAnchor
                                        : QTextCursor::MoveMode::MoveAnchor);
             this->ui_.textEdit->setTextCursor(cursor);
             return "";
         }},
        {"cursorToEnd",
         [this](const std::vector<QString> &arguments) -> QString {
             if (arguments.size() != 1)
             {
                 qCWarning(chatterinoHotkeys)
                     << "Invalid cursorToEnd arguments. Argument 0: select "
                        "(\"withSelection\" or \"withoutSelection\")";
                 return "Invalid cursorToEnd arguments. Argument 0: select "
                        "(\"withSelection\" or \"withoutSelection\")";
             }
             QTextCursor cursor = this->ui_.textEdit->textCursor();
             auto place = QTextCursor::End;
             const auto &stringTakeSelection = arguments.at(0);
             bool select{};
             if (stringTakeSelection == "withSelection")
             {
                 select = true;
             }
             else if (stringTakeSelection == "withoutSelection")
             {
                 select = false;
             }
             else
             {
                 qCWarning(chatterinoHotkeys)
                     << "Invalid cursorToEnd select argument (0)!";
                 return "Invalid cursorToEnd select argument (0)!";
             }

             cursor.movePosition(place,
                                 select ? QTextCursor::MoveMode::KeepAnchor
                                        : QTextCursor::MoveMode::MoveAnchor);
             this->ui_.textEdit->setTextCursor(cursor);
             return "";
         }},
        {"openEmotesPopup",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             this->openEmotePopup();
             return "";
         }},
        {"sendMessage",
         [this](const std::vector<QString> &arguments) -> QString {
             return this->handleSendMessage(arguments);
         }},
        {"previousMessage",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             if (this->prevMsg_.isEmpty() || this->prevIndex_ == 0)
             {
                 return "";
             }

             if (this->prevIndex_ == (this->prevMsg_.size()))
             {
                 this->currMsg_ = this->ui_.textEdit->toPlainText();
             }

             this->prevIndex_--;
             this->ui_.textEdit->setPlainText(
                 this->prevMsg_.at(this->prevIndex_));
             this->ui_.textEdit->resetCompletion();

             QTextCursor cursor = this->ui_.textEdit->textCursor();
             cursor.movePosition(QTextCursor::End);
             this->ui_.textEdit->setTextCursor(cursor);

             return "";
         }},
        {"nextMessage",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             // If user did not write anything before then just do nothing.
             if (this->prevMsg_.isEmpty())
             {
                 return "";
             }
             bool cursorToEnd = true;
             QString message = this->ui_.textEdit->toPlainText();

             if (this->prevIndex_ != (this->prevMsg_.size() - 1) &&
                 this->prevIndex_ != this->prevMsg_.size())
             {
                 this->prevIndex_++;
                 this->ui_.textEdit->setPlainText(
                     this->prevMsg_.at(this->prevIndex_));
                 this->ui_.textEdit->resetCompletion();
             }
             else
             {
                 this->prevIndex_ = this->prevMsg_.size();
                 if (message == this->prevMsg_.at(this->prevIndex_ - 1))
                 {
                     // If user has just come from a message history
                     // Then simply get currMsg_.
                     this->ui_.textEdit->setPlainText(this->currMsg_);
                     this->ui_.textEdit->resetCompletion();
                 }
                 else if (message != this->currMsg_)
                 {
                     // If user are already in current message
                     // And type something new
                     // Then replace currMsg_ with new one.
                     this->currMsg_ = message;
                 }
                 // If user is already in current message
                 // Then don't touch cursos.
                 cursorToEnd =
                     (message == this->prevMsg_.at(this->prevIndex_ - 1));
             }

             if (cursorToEnd)
             {
                 QTextCursor cursor = this->ui_.textEdit->textCursor();
                 cursor.movePosition(QTextCursor::End);
                 this->ui_.textEdit->setTextCursor(cursor);
             }
             return "";
         }},
        {"undo",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             this->ui_.textEdit->undo();
             return "";
         }},
        {"redo",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             this->ui_.textEdit->redo();
             return "";
         }},
        {"copy",
         [this](const std::vector<QString> &arguments) -> QString {
             // XXX: this action is unused at the moment, a qt standard shortcut is used instead
             if (arguments.empty())
             {
                 return "copy action takes only one argument: the source "
                        "of the copy \"split\", \"input\" or "
                        "\"auto\". If the source is \"split\", only text "
                        "from the chat will be copied. If it is "
                        "\"splitInput\", text from the input box will be "
                        "copied. Automatic will pick whichever has a "
                        "selection";
             }

             bool copyFromSplit = false;
             const auto &mode = arguments.at(0);
             if (mode == "split")
             {
                 copyFromSplit = true;
             }
             else if (mode == "splitInput")
             {
                 copyFromSplit = false;
             }
             else if (mode == "auto")
             {
                 const auto &cursor = this->ui_.textEdit->textCursor();
                 copyFromSplit = !cursor.hasSelection();
             }

             if (copyFromSplit)
             {
                 if (auto *pinnedView =
                         this->split_->pinnedExpandedMessageView();
                     pinnedView != nullptr && pinnedView->hasSelection())
                 {
                     pinnedView->copySelectedText();
                 }
                 else
                 {
                     this->channelView_->copySelectedText();
                 }
             }
             else
             {
                 this->ui_.textEdit->copy();
             }
             return "";
         }},
        {"paste",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             this->ui_.textEdit->paste();
             return "";
         }},
        {"clear",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             this->clearInput();
             return "";
         }},
        {"selectAll",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             this->ui_.textEdit->selectAll();
             return "";
         }},
        {"selectWord",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             auto cursor = this->ui_.textEdit->textCursor();
             cursor.select(QTextCursor::WordUnderCursor);
             this->ui_.textEdit->setTextCursor(cursor);
             return "";
         }},
        {"toggleTranslateOnSend",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             const auto newMode =
                 normalizedOutgoingTranslationMode(
                     this->outgoingTranslationMode()) ==
                         QLatin1String(OUTGOING_TRANSLATION_SEND)
                     ? QStringLiteral("off")
                     : QStringLiteral("send");

             getSettings()->setOutgoingTranslationModeForChannel(
                 this->outgoingTranslationChannelName(), newMode);
             this->updateOutgoingTranslationButton();
             this->updateOutgoingTranslationPreview();

             if (this->split_ != nullptr)
             {
                 if (auto channel = this->split_->getSelectedChannel())
                 {
                     channel->addSystemMessage(
                         QStringLiteral("Outgoing translation: %1")
                             .arg(outgoingTranslationModeLabel(newMode)));
                 }
             }

             return "";
         }},
    };

    this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
        HotkeyCategory::SplitInput, actions, this->parentWidget());
}

bool SplitInput::eventFilter(QObject *obj, QEvent *event)
{
    const auto isTextEditObject =
        obj == this->ui_.textEdit || (this->ui_.textEdit != nullptr &&
                                      obj == this->ui_.textEdit->viewport());
    if (isTextEditObject && event->type() == QEvent::KeyPress)
    {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        const auto key = keyEvent->key();
        if (key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_Tab ||
            key == Qt::Key_Backtab || key == Qt::Key_Escape)
        {
            if (this->handleCommandCompletionKey(keyEvent))
            {
                keyEvent->accept();
                return true;
            }
        }
    }

    if (isTextEditObject && event->type() == QEvent::ShortcutOverride &&
        this->commandCompletionSession_.active &&
        !this->commandCompletionSuggestions_.empty() &&
        this->ui_.commandCompletionWidget != nullptr &&
        this->ui_.commandCompletionWidget->isVisible())
    {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        const auto key = keyEvent->key();
        if ((key == Qt::Key_Up || key == Qt::Key_Down) &&
            keyEvent->modifiers() == Qt::NoModifier)
        {
            event->accept();
            return true;
        }
    }

    if (event->type() == QEvent::ShortcutOverride ||
        event->type() == QEvent::Shortcut)
    {
        if (auto *popup = this->inputCompletionPopup_.data())
        {
            if (popup->isVisible())
            {
                // Stop shortcut from triggering by saying we will handle it ourselves
                event->accept();

                // Return false means the underlying event isn't stopped, it will continue to propagate
                return false;
            }
        }
    }

    if (obj == this->ui_.channelPointsLabel &&
        event->type() == QEvent::MouseButtonPress)
    {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton)
        {
            auto *tc = dynamic_cast<TwitchChannel *>(
                this->split_->getSelectedChannel().get());
            if (!tc || !tc->shouldShowChannelPoints() ||
                !this->channelPointsLabelWanted_)
            {
                return true;
            }

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
            if (getSettings()->openRewardsWithChannelPointsClick)
            {
                ChannelPointsDialog::showDialog(tc, this, this->split_);
                return true;
            }
#endif
            const auto now = QDateTime::currentMSecsSinceEpoch();
            if (tc->isChannelPointsFetchInFlight())
            {
                this->channelPointsManualRefreshLoading_ = true;
            }
            else if (this->lastManualChannelPointsRefreshMs_ > 0 &&
                     now - this->lastManualChannelPointsRefreshMs_ <
                         CHANNEL_POINTS_MANUAL_REFRESH_COOLDOWN_MS)
            {
                return true;
            }
            else
            {
                this->lastManualChannelPointsRefreshMs_ = now;
                this->channelPointsManualRefreshLoading_ = true;
                tc->refreshChannelPointsIfStale(true);
            }

            this->ui_.channelPointsLabel->setText("...");
            this->ui_.channelPointsLabel->setToolTip(
                "Channel Points: refreshing...");
            this->updateActionRowCompactness();

            if (!tc->isChannelPointsFetchInFlight())
            {
                this->channelPointsManualRefreshLoading_ = false;
                this->updateChannelPointsDisplay(tc);
            }
            return true;
        }
    }

    if (event->type() == QEvent::Wheel &&
        (obj == this->ui_.commandCompletionWidget ||
         std::ranges::find(this->ui_.commandCompletionRows, obj) !=
             this->ui_.commandCompletionRows.end()))
    {
        auto *wheelEvent = static_cast<QWheelEvent *>(event);
        const auto delta = !wheelEvent->angleDelta().isNull()
                               ? wheelEvent->angleDelta().y()
                               : wheelEvent->pixelDelta().y();
        if (delta != 0 &&
            this->moveCommandCompletionSelection(delta < 0 ? 1 : -1))
        {
            return true;
        }
    }

    if (event->type() == QEvent::MouseButtonPress)
    {
        for (auto *row : this->ui_.commandCompletionRows)
        {
            if (obj != row)
            {
                continue;
            }

            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() != Qt::LeftButton)
            {
                return false;
            }

            const auto actualIndex =
                row->property("commandCompletionIndex").toInt();
            if (actualIndex >= 0 &&
                actualIndex < int(this->commandCompletionSuggestions_.size()))
            {
                this->commandCompletionSelectedIndex_ = actualIndex;
                this->insertCommandCompletionText(
                    this->commandCompletionSuggestions_[actualIndex].completion,
                    true);
                return true;
            }
        }
    }
    return BaseWidget::eventFilter(obj, event);
}

void SplitInput::installTextEditEvents()
{
    // We can safely ignore this signal's connection because SplitInput owns
    // the textEdit object, so it will always be deleted before SplitInput
    std::ignore =
        this->ui_.textEdit->keyPressed.connect([this](QKeyEvent *event) {
            if (this->handleCommandCompletionKey(event))
            {
                event->accept();
                return;
            }

            if (auto *popup = this->inputCompletionPopup_.data())
            {
                if (popup->isVisible())
                {
                    if (popup->eventFilter(nullptr, event))
                    {
                        event->accept();
                        return;
                    }
                }
            }

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
            if (this->channelPointRewardPromptSubmit_ &&
                event->key() == Qt::Key_Escape)
            {
                this->hideChannelPointRewardPrompt();
                event->accept();
                return;
            }
#endif

            // One of the last remaining of it's kind, the copy shortcut.
            // For some bizarre reason Qt doesn't want this key be rebound.
            // TODO(Mm2PL): Revisit in Qt6, maybe something changed?
            if ((event->key() == Qt::Key_C || event->key() == Qt::Key_Insert) &&
                event->modifiers() == Qt::ControlModifier)
            {
                if (this->channelView_->hasSelection())
                {
                    this->channelView_->copySelectedText();
                    event->accept();
                }
            }
        });

    std::ignore = this->ui_.textEdit->contextMenuRequested.connect(
        [this](QMenu *menu, QPoint pos) {
            auto channel = this->split_->getChannel();
            if (auto *mc = dynamic_cast<MultiChannel *>(channel.get()))
            {
                auto channels = mc->channels();
                auto currentIdx = mc->activeChannelIndex();
                if (!channels.empty())
                {
                    auto *submenu = menu->addMenu("Set Context");
                    auto *group = new QActionGroup(submenu);

                    for (size_t i = 0; i < channels.size(); i++)
                    {
                        QString name = channels[i].channel->getName() % u" (";
                        name +=
                            qmagicenum::enumNameString(channels[i].platform);
                        name += ')';
                        auto *action = new QAction(name, submenu);
                        action->setActionGroup(group);
                        action->setCheckable(true);
                        action->setChecked(i == currentIdx);
                        QObject::connect(
                            action, &QAction::toggled, this,
                            [this, i](bool checked) {
                                if (!checked)
                                {
                                    return;
                                }
                                auto *mc = dynamic_cast<MultiChannel *>(
                                    this->split_->getChannel().get());
                                mc->setActiveChannelIndex(i);
                                getApp()
                                    ->getWindows()
                                    ->forceLayoutChannelViews();
                            });
                        submenu->addAction(action);
                    }
                }
            }

#ifdef CHATTERINO_WITH_SPELLCHECK
            menu->addSeparator();
            auto *spellcheckAction = new QAction("Check spelling", menu);
            spellcheckAction->setCheckable(true);
            spellcheckAction->setChecked(this->shouldCheckSpelling());
            QObject::connect(spellcheckAction, &QAction::toggled, this,
                             [this](bool enabled) {
                                 this->checkSpellingOverride_ = enabled;
                                 this->checkSpellingChanged();
                             });
            menu->addAction(spellcheckAction);

            int nSuggestions = getSettings()->nSpellCheckingSuggestions;
            if (nSuggestions < 0)
            {
                nSuggestions = std::numeric_limits<int>::max();
            }

            if (!this->inputHighlighter || nSuggestions == 0)
            {
                return;
            }

            auto cursorAtPos = this->ui_.textEdit->cursorForPosition(pos);
            QString text = this->ui_.textEdit->toPlainText();
            QStringView word =
                this->inputHighlighter->getWordAt(text, cursorAtPos.position());
            if (!word.isEmpty())
            {
                auto cursor = this->ui_.textEdit->textCursor();
                // Select `word`. `word` is a view into `text`, so we can use
                // the offsets of `word` from the start of `text`.
                cursor.setPosition(
                    static_cast<int>(word.begin() - text.begin()));
                cursor.setPosition(static_cast<int>(word.end() - text.begin()),
                                   QTextCursor::KeepAnchor);

                auto suggestions =
                    getApp()->getSpellChecker()->suggestions(word.toString());
                for (const auto &sugg :
                     suggestions | std::views::take(nSuggestions))
                {
                    auto qSugg = QString::fromStdString(sugg);
                    menu->addAction(qSugg, [this, qSugg, cursor]() mutable {
                        cursor.insertText(qSugg);
                        this->ui_.textEdit->setTextCursor(cursor);
                    });
                }
            }
#else
            (void)menu;
            (void)pos;
            (void)this;
#endif
        });
}

void SplitInput::mousePressEvent(QMouseEvent *event)
{
    this->giveFocus(Qt::MouseFocusReason);

    if (this->hidden)
    {
        BaseWidget::mousePressEvent(event);
    }
    // else, don't call QWidget::mousePressEvent,
    // which will call event->ignore()
}

void SplitInput::onTextChanged()
{
    this->updateCompletionPopup();
}

void SplitInput::bindNukePreviewChannel()
{
    this->nukePreviewMessageConnection_ = pajlada::Signals::ScopedConnection();
    this->nukePreviewReplaceConnection_ = pajlada::Signals::ScopedConnection();
    this->nukePreviewClearConnection_ = pajlada::Signals::ScopedConnection();

    auto channel = this->split_->getSelectedChannel();
    if (channel == nullptr)
    {
        return;
    }

    this->nukePreviewMessageConnection_ = channel->messageAppended.connect(
        [this](MessagePtr &, std::optional<MessageFlags>) {
            this->scheduleNukePreviewRefresh();
        });
    this->nukePreviewReplaceConnection_ = channel->messageReplaced.connect(
        [this](size_t, const MessagePtr &, const MessagePtr &) {
            this->scheduleNukePreviewRefresh();
        });
    this->nukePreviewClearConnection_ =
        channel->messagesCleared.connect([this] {
            this->scheduleNukePreviewRefresh();
        });
}

void SplitInput::scheduleNukePreviewRefresh()
{
    if (!this->nukePreviewCommandActive_ || !getSettings()->nukePreviewEnabled)
    {
        return;
    }

    if (!this->nukePreviewTimer_.isActive())
    {
        this->nukePreviewTimer_.start();
    }
}

void SplitInput::updateNukePreview(const QString &text)
{
    this->pendingNukePreviewText_ = text;
    const auto trimmed = text.trimmed();
    const bool isNukeCommand =
        trimmed.compare(QStringLiteral("/nuke"), Qt::CaseInsensitive) == 0 ||
        trimmed.startsWith(QStringLiteral("/nuke "), Qt::CaseInsensitive);
    this->nukePreviewCommandActive_ =
        getSettings()->nukePreviewEnabled && isNukeCommand;
    if (!getSettings()->nukePreviewEnabled || !isNukeCommand)
    {
        this->nukePreviewTimer_.stop();
        this->applyNukePreview();
        return;
    }

    this->nukePreviewTimer_.start();
}

void SplitInput::applyNukePreview()
{
    if (this->channelView_ == nullptr)
    {
        return;
    }

    const auto preview = commands::buildNukePreview(
        this->pendingNukePreviewText_, this->split_->getSelectedChannel());
    if (!preview.active)
    {
        this->nukePreviewCommandActive_ = false;
        this->channelView_->clearNukePreview();
        this->ui_.nukePreviewLabel->hide();
        this->ui_.nukePreviewLabel->clear();
        return;
    }

    this->nukePreviewCommandActive_ = true;
    this->channelView_->setNukePreviewMessageIds(preview.messageIDs);
    this->ui_.nukePreviewLabel->setText(preview.statusText);
    this->ui_.nukePreviewLabel->setVisible(!preview.statusText.isEmpty());
}

void SplitInput::bindRaidStatusChannel()
{
    this->raidStatusConnection_ = pajlada::Signals::ScopedConnection();

    auto *channel =
        dynamic_cast<TwitchChannel *>(this->split_->getSelectedChannel().get());
    if (channel == nullptr)
    {
        return;
    }

    this->raidStatusConnection_ = channel->raidChanged.connect([this] {
        this->updateRaidStatus();
    });
}

void SplitInput::updateRaidStatus()
{
    auto *channel =
        dynamic_cast<TwitchChannel *>(this->split_->getSelectedChannel().get());
    if (channel == nullptr || !getSettings()->showRaidStatusAboveInput)
    {
        this->raidStatusTimer_.stop();
        this->raidStatusProgressAnimation_.stop();
        this->ui_.raidStatusWidget->hide();
        return;
    }

    std::optional<TwitchChannel::RaidEvent> raid;
    {
        auto locked = channel->accessRaid();
        if (locked->has_value())
        {
            raid = **locked;
        }
    }

    if (!raid)
    {
        this->raidStatusTimer_.stop();
        this->raidStatusProgressAnimation_.stop();
        this->ui_.raidStatusWidget->hide();
        return;
    }

    const auto remainingMs = remainingRaidMilliseconds(*raid);
    if (remainingMs <= 0)
    {
        this->raidStatusTimer_.stop();
        this->raidStatusProgressAnimation_.stop();
        this->ui_.raidStatusWidget->hide();
        channel->setActiveRaid(std::nullopt);
        return;
    }

    const auto totalSeconds = std::max(1, raid->forceRaidNowSeconds);
    const auto totalMs = qint64(totalSeconds) * 1000;
    const auto remainingSeconds = int((remainingMs + 999) / 1000);
    const auto elapsedMs =
        std::clamp(totalMs - remainingMs, qint64(0), totalMs);
    const auto nextElapsedMs = std::clamp(
        elapsedMs + RAID_STATUS_PROGRESS_TICK_MS, qint64(0), totalMs);
    const auto elapsedSeconds =
        std::clamp(int(elapsedMs / 1000), 0, totalSeconds);

    auto target = raid->targetDisplayName.isEmpty() ? raid->targetLogin
                                                    : raid->targetDisplayName;
    if (target.isEmpty())
    {
        target = QStringLiteral("target");
    }
    const auto phase = totalSeconds >= 90 && elapsedSeconds < 10
                           ? QStringLiteral("Preparing raid to %1")
                           : QStringLiteral("Raiding %1 in %2");
    const auto text =
        totalSeconds >= 90 && elapsedSeconds < 10
            ? QStringLiteral("%1 - %2 - %3")
                  .arg(phase.arg(target), formatRaidCountdown(remainingSeconds),
                       raidViewerText(raid->viewerCount))
            : QStringLiteral("%1 - %2").arg(
                  phase.arg(target, formatRaidCountdown(remainingSeconds)),
                  raidViewerText(raid->viewerCount));

    if (this->ui_.raidStatusLabel->text() != text)
    {
        this->ui_.raidStatusLabel->setText(text);
    }
    const auto currentProgress = raidProgressValue(elapsedMs, totalMs);
    const auto nextProgress = raidProgressValue(nextElapsedMs, totalMs);
    const bool wasVisible = this->ui_.raidStatusWidget->isVisible();
    if (!wasVisible)
    {
        this->ui_.raidStatusProgress->setValue(currentProgress);
    }
    this->ui_.raidStatusWidget->show();

    const auto animationMs = int(std::clamp(
        remainingMs, qint64(1), qint64(RAID_STATUS_PROGRESS_TICK_MS)));
    this->raidStatusProgressAnimation_.stop();
    this->raidStatusProgressAnimation_.setDuration(animationMs);
    this->raidStatusProgressAnimation_.setStartValue(
        this->ui_.raidStatusProgress->value());
    this->raidStatusProgressAnimation_.setEndValue(nextProgress);
    this->raidStatusProgressAnimation_.start();

    if (!this->raidStatusTimer_.isActive())
    {
        this->raidStatusTimer_.start();
    }
}

void SplitInput::onCursorPositionChanged()
{
    this->updateCompletionPopup();
}

QString SplitInput::currentOutgoingMessageBody() const
{
    auto message = this->ui_.textEdit->toPlainText();

    if (this->enableInlineReplying_ && this->replyTarget_ != nullptr)
    {
        message.remove(0, this->replyTarget_->displayName.length() + 1);
        if (!message.isEmpty() && message.at(0) == ' ')
        {
            message.remove(0, 1);
        }
    }

    return message.replace('\n', ' ');
}

QString SplitInput::outgoingTranslationChannelName() const
{
    if (this->split_ == nullptr)
    {
        return {};
    }

    const auto channel = this->split_->getSelectedChannel();
    return channel == nullptr ? QString{} : channel->getName();
}

QString SplitInput::outgoingTranslationMode() const
{
    return getSettings()->outgoingTranslationModeForChannel(
        this->outgoingTranslationChannelName());
}

QString SplitInput::outgoingTranslationTargetLanguage() const
{
    return getSettings()->outgoingTranslationTargetLanguageForChannel(
        this->outgoingTranslationChannelName());
}

bool SplitInput::shouldTranslateOutgoingMessage(const QString &message) const
{
    const auto trimmed = message.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith('/'))
    {
        return false;
    }

    auto channel = this->split_->getSelectedChannel();
    return channel != nullptr && channel->isTwitchOrKickChannel();
}

void SplitInput::clearOutgoingTranslationPreview()
{
    this->outgoingTranslationPreviewTimer_.stop();
    this->pendingOutgoingTranslationText_.clear();
    this->outgoingTranslationPreviewSource_.clear();
    this->outgoingTranslationPreviewTarget_.clear();
    this->outgoingTranslationPreviewText_.clear();
    ++this->outgoingTranslationGeneration_;

    if (this->ui_.translationPreviewWidget != nullptr)
    {
        this->ui_.translationPreviewWidget->hide();
    }
    if (this->ui_.translationPreviewLabel != nullptr)
    {
        this->ui_.translationPreviewLabel->clear();
    }
}

void SplitInput::scheduleOutgoingTranslationPreview(const QString &text)
{
    this->pendingOutgoingTranslationText_ = text;
    this->outgoingTranslationPreviewTimer_.start();
}

void SplitInput::updateOutgoingTranslationPreview()
{
    const auto mode =
        normalizedOutgoingTranslationMode(this->outgoingTranslationMode());
    if (mode == QLatin1String(OUTGOING_TRANSLATION_OFF) ||
        this->ui_.translationPreviewWidget == nullptr)
    {
        this->clearOutgoingTranslationPreview();
        return;
    }

    const auto body = this->currentOutgoingMessageBody().trimmed();
    if (!this->shouldTranslateOutgoingMessage(body))
    {
        this->clearOutgoingTranslationPreview();
        return;
    }

    const auto targetLanguage = normalizedTranslationTargetLanguage(
        this->outgoingTranslationTargetLanguage());
    const auto targetName = translationLanguageName(targetLanguage);

    if (this->outgoingTranslationPreviewSource_ == body &&
        this->outgoingTranslationPreviewTarget_ == targetLanguage &&
        !this->outgoingTranslationPreviewText_.isEmpty())
    {
        this->ui_.translationPreviewLabel->setText(QStringLiteral("%1: %2").arg(
            targetName, this->outgoingTranslationPreviewText_));
        this->ui_.translationPreviewWidget->show();
        return;
    }

    this->ui_.translationPreviewLabel->setText(
        QStringLiteral("Translating to %1...").arg(targetName));
    this->ui_.translationPreviewWidget->show();
    this->scheduleOutgoingTranslationPreview(body);
}

void SplitInput::applyOutgoingTranslationPreview()
{
    const auto body = this->pendingOutgoingTranslationText_.trimmed();
    if (!this->shouldTranslateOutgoingMessage(body))
    {
        this->clearOutgoingTranslationPreview();
        return;
    }

    const auto targetLanguage = normalizedTranslationTargetLanguage(
        this->outgoingTranslationTargetLanguage());
    const auto targetName = translationLanguageName(targetLanguage);
    const auto generation = ++this->outgoingTranslationGeneration_;
    requestTextTranslation(
        body, targetLanguage, this,
        [this, generation, body, targetLanguage,
         targetName](const TranslationResult &result) {
            if (generation != this->outgoingTranslationGeneration_ ||
                body != this->currentOutgoingMessageBody().trimmed() ||
                targetLanguage !=
                    normalizedTranslationTargetLanguage(
                        this->outgoingTranslationTargetLanguage()))
            {
                return;
            }

            this->outgoingTranslationPreviewSource_ = body;
            this->outgoingTranslationPreviewTarget_ = targetLanguage;
            this->outgoingTranslationPreviewText_ =
                result.translatedText.trimmed();
            this->ui_.translationPreviewLabel->setText(
                QStringLiteral("%1: %2").arg(
                    targetName, this->outgoingTranslationPreviewText_));
            this->ui_.translationPreviewWidget->show();
        },
        [this, generation](const QString &) {
            if (generation != this->outgoingTranslationGeneration_)
            {
                return;
            }
            this->ui_.translationPreviewLabel->setText(
                QStringLiteral("Could not translate this draft."));
            this->ui_.translationPreviewWidget->show();
        },
        [] {});
}

void SplitInput::updateOutgoingTranslationButton()
{
    if (this->ui_.outgoingTranslateButton == nullptr)
    {
        return;
    }

    const auto mode =
        normalizedOutgoingTranslationMode(this->outgoingTranslationMode());
    const auto targetLanguage = normalizedTranslationTargetLanguage(
        this->outgoingTranslationTargetLanguage());
    const auto targetName = translationLanguageName(targetLanguage);

    std::optional<QColor> buttonColor;
    if (mode != QLatin1String(OUTGOING_TRANSLATION_OFF))
    {
        buttonColor = QColor(255, 138, 0);
    }
    this->ui_.outgoingTranslateButton->setColor(buttonColor);
    this->ui_.outgoingTranslateButton->setToolTip(
        QStringLiteral("Outgoing translation: %1\nTarget: %2")
            .arg(outgoingTranslationModeLabel(mode), targetName));
}

void SplitInput::openOutgoingTranslationMenu()
{
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    auto *modeGroup = new QActionGroup(menu);
    modeGroup->setExclusive(true);
    auto addMode = [&](const QString &label, const QString &value) {
        auto *action = menu->addAction(label);
        action->setCheckable(true);
        action->setChecked(normalizedOutgoingTranslationMode(
                               this->outgoingTranslationMode()) == value);
        modeGroup->addAction(action);
        QObject::connect(action, &QAction::triggered, this, [this, value] {
            getSettings()->setOutgoingTranslationModeForChannel(
                this->outgoingTranslationChannelName(), value);
            this->updateOutgoingTranslationButton();
            this->updateOutgoingTranslationPreview();
        });
    };

    addMode("Off", QStringLiteral("off"));
    addMode("Preview only", QStringLiteral("preview"));
    addMode("Translate on send", QStringLiteral("send"));
    menu->addSeparator();

    auto *targetMenu = menu->addMenu("Target language");
    auto *languageGroup = new QActionGroup(targetMenu);
    languageGroup->setExclusive(true);
    const auto currentTarget = normalizedTranslationTargetLanguage(
        this->outgoingTranslationTargetLanguage());

    auto addLanguageAction = [&](QMenu *parent,
                                 const TranslationLanguage &language) {
        auto *action = parent->addAction(language.name);
        action->setCheckable(true);
        action->setChecked(language.code == currentTarget);
        languageGroup->addAction(action);
        QObject::connect(
            action, &QAction::triggered, this, [this, code = language.code] {
                getSettings()->setOutgoingTranslationTargetLanguageForChannel(
                    this->outgoingTranslationChannelName(), code);
                this->outgoingTranslationPreviewSource_.clear();
                this->updateOutgoingTranslationButton();
                this->updateOutgoingTranslationPreview();
            });
    };

    static const std::vector<QString> commonLanguages{
        QStringLiteral("en"),    QStringLiteral("es"),    QStringLiteral("pt"),
        QStringLiteral("fr"),    QStringLiteral("de"),    QStringLiteral("it"),
        QStringLiteral("nl"),    QStringLiteral("pl"),    QStringLiteral("tr"),
        QStringLiteral("ru"),    QStringLiteral("ja"),    QStringLiteral("ko"),
        QStringLiteral("zh-cn"), QStringLiteral("zh-tw"), QStringLiteral("ar"),
    };
    const auto isCommonLanguage = [](const QString &code) {
        return std::ranges::find(commonLanguages, code) !=
               commonLanguages.end();
    };

    const auto &languages = supportedTranslationLanguages();
    for (const auto &code : commonLanguages)
    {
        const auto it =
            std::ranges::find_if(languages, [&](const auto &language) {
                return language.code == code;
            });
        if (it != languages.end())
        {
            addLanguageAction(targetMenu, *it);
        }
    }

    targetMenu->addSeparator();
    auto *moreMenu = targetMenu->addMenu("More languages");
    auto *aToFMenu = moreMenu->addMenu("A - F");
    auto *gToMMenu = moreMenu->addMenu("G - M");
    auto *nToZMenu = moreMenu->addMenu("N - Z");
    for (const auto &language : languages)
    {
        if (isCommonLanguage(language.code))
        {
            continue;
        }

        const auto first = language.name.isEmpty()
                               ? QChar('N')
                               : language.name.front().toUpper();
        if (first <= QChar('F'))
        {
            addLanguageAction(aToFMenu, language);
        }
        else if (first <= QChar('M'))
        {
            addLanguageAction(gToMMenu, language);
        }
        else
        {
            addLanguageAction(nToZMenu, language);
        }
    }

    menu->popup(this->ui_.outgoingTranslateButton->mapToGlobal(
        QPoint(0, this->ui_.outgoingTranslateButton->height())));
}

void SplitInput::updateCompletionPopup()
{
    if (this->updatingCommandCompletionText_)
    {
        return;
    }

    auto *channel = this->split_->getSelectedChannel().get();
    auto *tc = dynamic_cast<TwitchChannel *>(channel);
    bool showEmoteCompletion = getSettings()->emoteCompletionWithColon;
    bool showUsernameCompletion =
        tc != nullptr && getSettings()->showUsernameCompletionMenu;
    bool showCommandCompletion =
        channel != nullptr && getSettings()->showCommandSuggestions;
    if (!showEmoteCompletion && !showUsernameCompletion &&
        !showCommandCompletion)
    {
        this->resetCommandCompletionSession();
        this->hideCompletionPopup();
        return;
    }

    // check if in completion prefix
    auto &edit = *this->ui_.textEdit;

    auto text = edit.toPlainText();
    auto cursorPosition = edit.textCursor().position();
    auto position = cursorPosition - 1;

    if (text.length() == 0 || position == -1)
    {
        this->resetCommandCompletionSession();
        this->hideCompletionPopup();
        return;
    }

    if (this->commandCompletionSession_.active &&
        this->commandCompletionSession_.inserted)
    {
        const auto start = this->commandCompletionSession_.start;
        const auto end = this->commandCompletionSession_.end;
        if (start >= 0 && end <= text.size() && start < end &&
            cursorPosition == end)
        {
            const auto currentCompletion = text.mid(start, end - start);
            const auto stillCycling = std::any_of(
                this->commandCompletionSuggestions_.begin(),
                this->commandCompletionSuggestions_.end(),
                [&](const auto &suggestion) {
                    return currentCompletion == suggestion.completion + ' ';
                });
            if (stillCycling)
            {
                this->hideCompletionPopup();
                this->renderCommandCompletion();
                return;
            }
        }
    }

    if (showCommandCompletion && cursorPosition > 0)
    {
        const auto beforeCursor = text.left(cursorPosition);
        qsizetype commandStart = 0;
        while (commandStart < beforeCursor.size() &&
               beforeCursor.at(commandStart).isSpace())
        {
            ++commandStart;
        }

        if (commandStart < beforeCursor.size() &&
            (beforeCursor.at(commandStart) == '/' ||
             beforeCursor.at(commandStart) == '.') &&
            !beforeCursor.mid(commandStart).contains(QChar(' ')) &&
            beforeCursor.size() - commandStart >= 2)
        {
            const auto query = beforeCursor.mid(commandStart);
            if (this->updateCommandCompletion(query, int(commandStart),
                                              cursorPosition))
            {
                return;
            }
        }
    }

    this->resetCommandCompletionSession();

    for (int i = std::clamp(position, 0, (int)text.length() - 1); i >= 0; i--)
    {
        if (text[i] == ' ')
        {
            this->hideCompletionPopup();
            return;
        }

        if (text[i] == ':' && showEmoteCompletion)
        {
            if (i == 0 || text[i - 1].isSpace())
            {
                this->showCompletionPopup(text.mid(i, position - i + 1),
                                          CompletionKind::Emote);
            }
            else
            {
                this->hideCompletionPopup();
            }
            return;
        }

        if (text[i] == '@' && showUsernameCompletion)
        {
            if (i == 0 || text[i - 1].isSpace())
            {
                this->showCompletionPopup(text.mid(i, position - i + 1),
                                          CompletionKind::User);
            }
            else
            {
                this->hideCompletionPopup();
            }
            return;
        }
    }

    this->hideCompletionPopup();
}

bool SplitInput::updateCommandCompletion(const QString &query, int start,
                                         int end)
{
    if (this->ui_.commandCompletionWidget == nullptr)
    {
        return false;
    }

    completion::CommandSource source(
        std::make_unique<completion::CommandStrategy>(false), nullptr,
        this->split_->getChannel().get());
    source.update(query);

    this->commandCompletionSuggestions_.clear();
    for (const auto &command : source.output())
    {
        const auto prefix =
            command.prefix.isEmpty() ? QStringLiteral("/") : command.prefix;
        this->commandCompletionSuggestions_.push_back({
            .completion = prefix + command.name,
            .usage = command.usage,
        });
    }

    if (this->commandCompletionSuggestions_.empty())
    {
        this->resetCommandCompletionSession();
        return false;
    }

    this->commandCompletionSelectedIndex_ = 0;
    this->commandCompletionSession_ = {
        .active = true,
        .inserted = false,
        .selectionChanged = false,
        .query = query,
        .start = start,
        .end = end,
    };
    this->hideCompletionPopup();
    this->renderCommandCompletion();
    return true;
}

void SplitInput::renderCommandCompletion()
{
    if (this->ui_.commandCompletionWidget == nullptr)
    {
        return;
    }

    const auto count = int(this->commandCompletionSuggestions_.size());
    if (count <= 0)
    {
        this->hideCommandCompletion();
        return;
    }

    this->commandCompletionSelectedIndex_ =
        std::clamp(this->commandCompletionSelectedIndex_, 0, count - 1);
    auto first = this->commandCompletionSelectedIndex_ - 1;
    first = std::clamp(
        first, 0,
        std::max(0, count - int(this->ui_.commandCompletionRows.size())));

    const auto textColor = this->theme->splits.input.text.name(QColor::HexRgb);
    auto muted = this->theme->splits.input.text;
    muted.setAlphaF(this->theme->isLightTheme() ? 0.68F : 0.58F);
    const auto mutedColor = muted.name(QColor::HexArgb);

    for (size_t rowIndex = 0; rowIndex < this->ui_.commandCompletionRows.size();
         ++rowIndex)
    {
        auto *row = this->ui_.commandCompletionRows[rowIndex];
        if (row == nullptr)
        {
            continue;
        }

        const auto actualIndex = first + int(rowIndex);
        if (actualIndex >= count)
        {
            row->hide();
            row->setProperty("commandCompletionIndex", -1);
            continue;
        }

        const auto &suggestion =
            this->commandCompletionSuggestions_[actualIndex];
        const auto selected =
            actualIndex == this->commandCompletionSelectedIndex_;
        row->setProperty("commandCompletionIndex", actualIndex);
        row->setStyleSheet(
            selected
                ? QStringLiteral(
                      "QLabel#commandCompletionRow { background: rgba(120, "
                      "120, 120, 0.20); border: 0; }")
                : QStringLiteral(
                      "QLabel#commandCompletionRow { background: transparent; "
                      "border: 0; }"));
        row->setText(QStringLiteral(
                         "<span style=\"font-weight:600; color:%1;\">%2</span>"
                         "<span style=\"color:%3;\">%4%5</span>")
                         .arg(textColor, suggestion.completion.toHtmlEscaped(),
                              mutedColor,
                              suggestion.usage.isEmpty() ? QString()
                                                         : QStringLiteral(" "),
                              suggestion.usage.toHtmlEscaped()));
        row->show();
    }

    this->ui_.commandCompletionWidget->show();
}

void SplitInput::hideCommandCompletion()
{
    if (this->ui_.commandCompletionWidget != nullptr)
    {
        this->ui_.commandCompletionWidget->hide();
    }
    for (auto *row : this->ui_.commandCompletionRows)
    {
        if (row != nullptr)
        {
            row->setProperty("commandCompletionIndex", -1);
            row->hide();
        }
    }
}

bool SplitInput::moveCommandCompletionSelection(int offset)
{
    const auto count = int(this->commandCompletionSuggestions_.size());
    if (!this->commandCompletionSession_.active || count <= 0 ||
        this->ui_.commandCompletionWidget == nullptr ||
        !this->ui_.commandCompletionWidget->isVisible())
    {
        return false;
    }

    this->commandCompletionSelectedIndex_ =
        (this->commandCompletionSelectedIndex_ + offset) % count;
    if (this->commandCompletionSelectedIndex_ < 0)
    {
        this->commandCompletionSelectedIndex_ += count;
    }
    this->commandCompletionSession_.selectionChanged = true;
    this->renderCommandCompletion();
    return true;
}

void SplitInput::showCompletionPopup(const QString &text, CompletionKind kind)
{
    if (this->inputCompletionPopup_.isNull())
    {
        this->inputCompletionPopup_ = new InputCompletionPopup(this);
        this->inputCompletionPopup_->setInputAction(
            [that = QPointer(this)](const QString &text) mutable {
                if (auto *this2 = that.data())
                {
                    this2->insertCompletionText(text);
                    this2->hideCompletionPopup();
                    this2->resetCommandCompletionSession();
                }
            });
    }

    auto *popup = this->inputCompletionPopup_.data();
    assert(popup);

    popup->updateCompletion(text, kind, this->split_->getSelectedChannel());
    if (!popup->hasResults())
    {
        return;
    }

    auto pos = this->mapToGlobal(QPoint{0, 0}) - QPoint(0, popup->height()) +
               QPoint((this->width() - popup->width()) / 2, 0);

    popup->move(pos);
    popup->show();
}

void SplitInput::hideCompletionPopup()
{
    if (auto *popup = this->inputCompletionPopup_.data())
    {
        popup->hide();
    }
}

void SplitInput::insertCompletionText(const QString &input_)
{
    auto &edit = *this->ui_.textEdit;
    auto input = input_ + ' ';

    auto text = edit.toPlainText();
    auto position = edit.textCursor().position() - 1;

    for (int i = std::clamp(position, 0, (int)text.length() - 1); i >= 0; i--)
    {
        bool done = false;
        if (text[i] == ':')
        {
            done = true;
        }
        else if (text[i] == '@')
        {
            const auto userMention =
                formatUserMention(input_, edit.isFirstWord(),
                                  getSettings()->mentionUsersWithComma);
            input = "@" + userMention + " ";
            done = true;
        }

        if (done)
        {
            auto cursor = edit.textCursor();
            edit.setPlainText(
                text.remove(i, position - i + 1).insert(i, input));

            cursor.setPosition(i + input.size());
            edit.setTextCursor(cursor);
            break;
        }
    }
}

bool SplitInput::handleCommandCompletionKey(QKeyEvent *event)
{
    if (!this->commandCompletionSession_.active ||
        this->commandCompletionSuggestions_.empty() ||
        this->ui_.commandCompletionWidget == nullptr ||
        !this->ui_.commandCompletionWidget->isVisible())
    {
        return false;
    }

    const auto key = event->key();
    if (key == Qt::Key_Enter || key == Qt::Key_Return)
    {
        this->resetCommandCompletionSession();
        return false;
    }

    if (key == Qt::Key_Escape)
    {
        this->resetCommandCompletionSession();
        return true;
    }

    if (key == Qt::Key_Down)
    {
        return this->moveCommandCompletionSelection(1);
    }

    if (key == Qt::Key_Up)
    {
        return this->moveCommandCompletionSelection(-1);
    }

    const auto isBacktab =
        key == Qt::Key_Backtab ||
        (key == Qt::Key_Tab && (event->modifiers() & Qt::ShiftModifier));
    if (key != Qt::Key_Tab && key != Qt::Key_Backtab)
    {
        return false;
    }

    if (!this->commandCompletionSession_.selectionChanged)
    {
        if (isBacktab)
        {
            this->moveCommandCompletionSelection(-1);
        }
        else if (this->commandCompletionSession_.inserted)
        {
            this->moveCommandCompletionSelection(1);
        }
    }

    this->commandCompletionSelectedIndex_ =
        std::clamp(this->commandCompletionSelectedIndex_, 0,
                   int(this->commandCompletionSuggestions_.size()) - 1);
    const auto completion = this->commandCompletionSuggestions_
                                [this->commandCompletionSelectedIndex_]
                                    .completion;
    if (completion.isEmpty())
    {
        return true;
    }

    this->insertCommandCompletionText(completion, true);
    return true;
}

void SplitInput::insertCommandCompletionText(const QString &completion,
                                             bool keepPopup)
{
    auto &edit = *this->ui_.textEdit;
    auto text = edit.toPlainText();
    auto start = this->commandCompletionSession_.start;
    auto end = this->commandCompletionSession_.end;

    if (!this->commandCompletionSession_.active)
    {
        const auto cursorPosition = edit.textCursor().position();
        const auto beforeCursor = text.left(cursorPosition);
        start = 0;
        while (start < beforeCursor.size() && beforeCursor.at(start).isSpace())
        {
            ++start;
        }
        end = cursorPosition;
    }

    const auto textSize = int(text.size());
    start = std::clamp(start, 0, textSize);
    end = std::clamp(end, start, textSize);

    const auto input = completion + ' ';
    auto cursor = edit.textCursor();
    this->updatingCommandCompletionText_ = true;
    edit.setPlainText(text.remove(start, end - start).insert(start, input));
    cursor.setPosition(start + input.size());
    edit.setTextCursor(cursor);
    this->updatingCommandCompletionText_ = false;

    if (keepPopup)
    {
        this->commandCompletionSession_.active = true;
        this->commandCompletionSession_.inserted = true;
        this->commandCompletionSession_.selectionChanged = false;
        this->commandCompletionSession_.start = start;
        this->commandCompletionSession_.end = start + int(input.size());
        this->renderCommandCompletion();
    }
    else
    {
        this->resetCommandCompletionSession();
    }
}

void SplitInput::resetCommandCompletionSession()
{
    this->commandCompletionSession_ = {};
    this->commandCompletionSuggestions_.clear();
    this->commandCompletionSelectedIndex_ = 0;
    this->hideCommandCompletion();
}

bool SplitInput::hasSelection() const
{
    return this->ui_.textEdit->textCursor().hasSelection();
}

void SplitInput::clearSelection() const
{
    auto cursor = this->ui_.textEdit->textCursor();
    cursor.clearSelection();
    this->ui_.textEdit->setTextCursor(cursor);
}

bool SplitInput::isEditFirstWord() const
{
    return this->ui_.textEdit->isFirstWord();
}

QString SplitInput::getInputText() const
{
    return this->ui_.textEdit->toPlainText();
}

void SplitInput::insertText(const QString &text)
{
    this->ui_.textEdit->insertPlainText(text);
}

void SplitInput::hide()
{
    if (this->isHidden())
    {
        return;
    }

    this->hidden = true;
    this->setMaximumHeight(0);
    this->relayoutParentWidgets();
}

void SplitInput::show()
{
    if (!this->isHidden())
    {
        return;
    }

    this->hidden = false;
    this->setMaximumHeight(this->scaledMaxHeight());
    this->relayoutParentWidgets();

    if (dynamic_cast<TwitchChannel *>(
            this->split_->getSelectedChannel().get()) != nullptr &&
        getSettings()->enableChannelPointsDisplay)
    {
        this->split_->scheduleDeferredTwitchRefresh(true);
    }
}

bool SplitInput::isHidden() const
{
    return this->hidden;
}

void SplitInput::setInputText(const QString &newInputText)
{
    this->ui_.textEdit->setPlainText(newInputText);
}

void SplitInput::editTextChanged()
{
    auto *app = getApp();

    // set textLengthLabel value
    QString text = this->ui_.textEdit->toPlainText();

    if (this->shouldPreventInput(text))
    {
        this->ui_.textEdit->setPlainText(text.left(TWITCH_MESSAGE_LIMIT));
        this->ui_.textEdit->moveCursor(QTextCursor::EndOfBlock);
        return;
    }

    if (text.startsWith("/r ", Qt::CaseInsensitive) &&
        this->split_->getSelectedChannel()->isTwitchChannel())
    {
        auto lastUser = app->getTwitch()->getLastUserThatWhisperedMe();
        if (!lastUser.isEmpty())
        {
            this->ui_.textEdit->setPlainText("/w " + lastUser + text.mid(2));
            this->ui_.textEdit->moveCursor(QTextCursor::EndOfBlock);
        }
    }
    else
    {
        this->textChanged.invoke(text);
        this->updateNukePreview(text);

        text = text.trimmed();
        text = app->getCommands()->execCommand(text, this->split_->getChannel(),
                                               true);
    }

    this->updateOutgoingTranslationPreview();

    if (text.length() > 0 &&
        getSettings()->messageOverflow.getValue() == MessageOverflow::Highlight)
    {
        QTextCursor cursor = this->ui_.textEdit->textCursor();
        QTextCharFormat format;
        QList<QTextEdit::ExtraSelection> selections;

        cursor.setPosition(qMin(text.length(), TWITCH_MESSAGE_LIMIT),
                           QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::Start, QTextCursor::KeepAnchor);
        selections.append({cursor, format});

        if (text.length() > TWITCH_MESSAGE_LIMIT)
        {
            cursor.setPosition(TWITCH_MESSAGE_LIMIT, QTextCursor::MoveAnchor);
            cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
            format.setForeground(Qt::red);
            selections.append({cursor, format});
        }
        // block reemit of QTextEdit::textChanged()
        {
            const QSignalBlocker b(this->ui_.textEdit);
            this->ui_.textEdit->setExtraSelections(selections);
        }
    }

    QString labelText;

    if (text.length() > 0 && getSettings()->showMessageLength)
    {
        labelText = QString::number(text.length());
        if (text.length() > TWITCH_MESSAGE_LIMIT)
        {
            this->ui_.textEditLength->setStyleSheet("color: red");
        }
        else
        {
            this->ui_.textEditLength->setStyleSheet("");
        }
    }
    else
    {
        labelText = "";
    }

    this->ui_.textEditLength->setText(labelText);
    this->updateActionRowCompactness();

    bool hasReply = false;
    if (this->enableInlineReplying_)
    {
        if (this->replyTarget_ != nullptr)
        {
            // Check if the input still starts with @username. If not, don't reply.
            //
            // We need to verify that
            // 1. the @username prefix exists and
            // 2. if a character exists after the @username, it is a space
            QString replyPrefix = "@" + this->replyTarget_->displayName;
            if (!text.startsWith(replyPrefix) ||
                (text.length() > replyPrefix.length() &&
                 text.at(replyPrefix.length()) != ' '))
            {
                this->clearReplyTarget();
            }
        }

        // Show/hide reply label if inline replies are possible
        hasReply = this->replyTarget_ != nullptr;
    }

    this->ui_.replyWrapper->setVisible(hasReply);
    this->ui_.replyLabel->setVisible(hasReply);
    this->ui_.cancelReplyButton->setVisible(hasReply);
}

void SplitInput::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);

    QColor borderColor =
        this->theme->isLightTheme() ? QColor("#ccc") : QColor("#333");

    QRect baseRect = this->rect();
    baseRect.setWidth(baseRect.width() - 1);

    auto *inputWrap = this->ui_.inputWrapper;
    auto inputBoxRect = inputWrap->geometry();
    inputBoxRect.setSize(inputBoxRect.size() - QSize{1, 1});

    painter.setBrush({this->theme->splits.input.background});
    painter.setPen(borderColor);
    painter.drawRect(inputBoxRect);

    if (this->enableInlineReplying_ && this->replyTarget_ != nullptr)
    {
        auto replyRect = this->ui_.replyWrapper->geometry();
        replyRect.setSize(replyRect.size() - QSize{1, 1});

        painter.setBrush(this->theme->splits.input.background);
        painter.setPen(borderColor);
        painter.drawRect(replyRect);

        QPoint replyLabelBorderStart(
            replyRect.x(),
            replyRect.y() + this->ui_.replyHbox->geometry().height());
        QPoint replyLabelBorderEnd(replyRect.right(),
                                   replyLabelBorderStart.y());
        painter.drawLine(replyLabelBorderStart, replyLabelBorderEnd);
    }
}

void SplitInput::resizeEvent(QResizeEvent *event)
{
    (void)event;

    if (this->height() == this->maximumHeight())
    {
        this->ui_.textEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    }
    else
    {
        this->ui_.textEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

    this->ui_.replyMessage->setWidth(this->replyMessageWidth());
    this->updateActionRowCompactness();
}

void SplitInput::giveFocus(Qt::FocusReason reason)
{
    this->ui_.textEdit->setFocus(reason);
}

void SplitInput::setReply(MessagePtr target, std::weak_ptr<Channel> channel)
{
    auto oldParent = this->replyTarget_;
    if (this->enableInlineReplying_ && oldParent)
    {
        // Remove old reply prefix
        auto replyPrefix = "@" + oldParent->displayName;
        auto plainText = this->ui_.textEdit->toPlainText().trimmed();
        if (plainText.startsWith(replyPrefix))
        {
            plainText.remove(0, replyPrefix.length());
        }
        this->ui_.textEdit->setPlainText(plainText.trimmed());
        this->ui_.textEdit->moveCursor(QTextCursor::EndOfBlock);
        this->ui_.textEdit->resetCompletion();
    }

    if (target != nullptr)
    {
        this->replyTarget_ = std::move(target);
        this->replyChannel_ = std::move(channel);

        if (this->enableInlineReplying_)
        {
            this->ui_.replyMessage->setWidth(this->replyMessageWidth());
            this->ui_.replyMessage->setMessage(this->replyTarget_);

            // add spacing between reply box and input box
            this->ui_.vbox->setSpacing(this->marginForTheme());
            if (!this->isHidden())
            {
                // update maximum height to give space for message
                this->setMaximumHeight(this->scaledMaxHeight());
            }

            // Only enable reply label if inline replying
            auto replyPrefix = "@" + this->replyTarget_->displayName;
            auto plainText = this->ui_.textEdit->toPlainText().trimmed();

            // This makes it so if plainText contains "@StreamerFan" and
            // we are replying to "@Streamer" we don't just leave "Fan"
            // in the text box
            if (plainText.startsWith(replyPrefix))
            {
                if (plainText.length() > replyPrefix.length())
                {
                    if (plainText.at(replyPrefix.length()) == ',' ||
                        plainText.at(replyPrefix.length()) == ' ')
                    {
                        plainText.remove(0, replyPrefix.length() + 1);
                    }
                }
                else
                {
                    plainText.remove(0, replyPrefix.length());
                }
            }
            if (!plainText.isEmpty() && !plainText.startsWith(' '))
            {
                replyPrefix.append(' ');
            }
            this->ui_.textEdit->setPlainText(replyPrefix + plainText + " ");
            this->ui_.textEdit->moveCursor(QTextCursor::EndOfBlock);
            this->ui_.textEdit->resetCompletion();
            this->ui_.replyLabel->setText("Replying to @" +
                                          this->replyTarget_->displayName);
            this->relayoutParentWidgets();
        }
    }
    else
    {
        this->replyTarget_.reset();
        this->replyChannel_.reset();

        if (this->enableInlineReplying_)
        {
            this->clearReplyTarget();
        }
    }
}

void SplitInput::setPlaceholderText(const QString &text)
{
    this->ui_.textEdit->setPlaceholderText(text);
}

void SplitInput::clearInput()
{
    this->currMsg_ = "";
    this->ui_.textEdit->setText("");
    this->ui_.textEdit->moveCursor(QTextCursor::Start);
    if (this->enableInlineReplying_)
    {
        this->clearReplyTarget();
    }
}

void SplitInput::clearReplyTarget()
{
    this->replyTarget_.reset();
    this->ui_.replyMessage->clearMessage();
    this->ui_.vbox->setSpacing(0);
    if (!this->isHidden())
    {
        this->setMaximumHeight(this->scaledMaxHeight());
    }
    this->relayoutParentWidgets();
}

bool SplitInput::shouldPreventInput(const QString &text) const
{
    if (getSettings()->messageOverflow.getValue() != MessageOverflow::Prevent)
    {
        return false;
    }

    auto channel = this->split_->getSelectedChannel();

    if (channel == nullptr)
    {
        return false;
    }

    if (!channel->isTwitchChannel())
    {
        // Don't respect this setting for IRC channels as the limits might be server-specific
        return false;
    }

    return text.length() > TWITCH_MESSAGE_LIMIT;
}

int SplitInput::marginForTheme() const
{
    if (this->theme->isLightTheme())
    {
        return int(3 * this->scale());
    }
    else
    {
        return int(1 * this->scale());
    }
}

void SplitInput::applyOuterMargin()
{
    auto margin = std::max(this->marginForTheme() - 1, 0);
    this->ui_.vbox->setContentsMargins(margin, margin, margin, margin);
}

int SplitInput::replyMessageWidth() const
{
    return this->ui_.inputWrapper->width() - 1 - 10;
}

void SplitInput::updateTextEditPalette()
{
    QPalette p;

    // Placeholder text color
    p.setColor(QPalette::PlaceholderText,
               this->theme->messages.textColors.chatPlaceholder);

    // Text color
    p.setColor(QPalette::Text, this->theme->messages.textColors.regular);

    // Selection background color
    p.setBrush(QPalette::Highlight,
               this->theme->isLightTheme()
                   ? QColor(u"#68B1FF"_s)
                   : this->theme->tabs.selected.backgrounds.regular);

    // Background color
    p.setBrush(QPalette::Base, this->backgroundColor());

    this->ui_.textEdit->setPalette(p);
}

QColor SplitInput::backgroundColor() const
{
    return this->backgroundColor_;
}

void SplitInput::setBackgroundColor(QColor newColor)
{
    this->backgroundColor_ = newColor;

    this->updateTextEditPalette();
}

std::optional<bool> SplitInput::checkSpellingOverride() const
{
    return this->checkSpellingOverride_;
}

void SplitInput::setCheckSpellingOverride(std::optional<bool> override)
{
    this->checkSpellingOverride_ = override;
    this->checkSpellingChanged();
}

bool SplitInput::shouldCheckSpelling() const
{
    if (this->checkSpellingOverride_)
    {
        return *this->checkSpellingOverride_;
    }
    return getSettings()->enableSpellChecking;
}

void SplitInput::checkSpellingChanged()
{
    QTextDocument *target = nullptr;
    if (this->shouldCheckSpelling())
    {
        target = this->ui_.textEdit->document();
    }

    if (this->inputHighlighter->document() != target)
    {
        this->inputHighlighter->setDocument(target);
    }
}

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
void SplitInput::showChannelPointRewardPrompt(
    const QString &title, const QString &placeholder, bool requireText,
    std::function<void(const QString &)> submitCallback)
{
    this->channelPointRewardPromptSubmit_ = std::move(submitCallback);
    this->channelPointRewardPromptRequiresText_ = requireText;
    this->ui_.channelPointRewardPromptTitle->setText(title);
    this->ui_.channelPointRewardPromptTitle->setToolTip(placeholder);
    this->ui_.channelPointRewardPromptWidget->show();
    this->ui_.textEdit->setFocus(Qt::OtherFocusReason);
    this->relayoutParentWidgets();
}

void SplitInput::hideChannelPointRewardPrompt()
{
    this->channelPointRewardPromptSubmit_ = nullptr;
    this->channelPointRewardPromptRequiresText_ = false;
    this->ui_.channelPointRewardPromptTitle->clear();
    this->ui_.channelPointRewardPromptTitle->setToolTip({});
    this->ui_.channelPointRewardPromptWidget->hide();
    this->relayoutParentWidgets();
}

bool SplitInput::submitChannelPointRewardPrompt()
{
    if (!this->channelPointRewardPromptSubmit_)
    {
        return false;
    }

    auto text = this->ui_.textEdit->toPlainText();
    text = text.replace('\n', ' ').trimmed();
    if (this->channelPointRewardPromptRequiresText_ && text.isEmpty())
    {
        return true;
    }

    auto callback = std::move(this->channelPointRewardPromptSubmit_);
    this->hideChannelPointRewardPrompt();
    this->clearInput();
    if (callback)
    {
        callback(text);
    }
    return true;
}
#endif

void SplitInput::bindChannelPoints(TwitchChannel *channel)
{
    this->channelPointSignal_ = pajlada::Signals::ScopedConnection();
    this->modStateSignal_ = pajlada::Signals::ScopedConnection();
    this->pollStateSignal_ = pajlada::Signals::ScopedConnection();
    this->focusedPointsConnection_ = pajlada::Signals::ScopedConnection();
    this->focusLostPointsConnection_ = pajlada::Signals::ScopedConnection();

    auto updateActionButtons = [this, channel]() {
        const bool canModerate = channel != nullptr &&
                                 (channel->isMod() || channel->isBroadcaster());
        const bool hasActivePoll = channel != nullptr && [&] {
            const auto poll = channel->accessPoll();
            return poll->has_value() &&
                   (*poll)->status.compare("ACTIVE", Qt::CaseInsensitive) == 0;
        }();
        this->predictionButtonWanted_ = getSettings()->enablePredictions &&
                                        getSettings()->showPredictionButton &&
                                        canModerate;
        this->pollButtonWanted_ = getSettings()->enablePolls &&
                                  getSettings()->showPollButton &&
                                  (canModerate || hasActivePoll);
        this->ui_.pollButton->setToolTip(hasActivePoll ? "Open poll"
                                                       : "Create poll");
        this->updateActionRowCompactness();
    };

    if (!channel)
    {
        this->clearChannelPointsDisplay();
        this->predictionButtonWanted_ = false;
        this->pollButtonWanted_ = false;
        this->updateActionRowCompactness();
        return;
    }

    updateActionButtons();
    this->modStateSignal_ =
        channel->userStateChanged.connect(updateActionButtons);
    this->pollStateSignal_ = channel->pollChanged.connect(updateActionButtons);

    if (!getSettings()->enableChannelPointsDisplay)
    {
        this->clearChannelPointsDisplay();
        return;
    }

    if (!(channel && getSettings()->enableChannelPointsDisplay))
    {
        this->clearChannelPointsDisplay();
        this->predictionButtonWanted_ = false;
        this->updateActionRowCompactness();
        return;
    }

    if (!channel->shouldShowChannelPoints())
    {
        this->clearChannelPointsDisplay();
        return;
    }

    this->channelPointsLabelWanted_ = true;
    if (this->ui_.channelPointsLabel->text().isEmpty())
    {
        this->ui_.channelPointsLabel->setText("...");
    }
    this->updateChannelPointsDisplay(channel);
    auto connectPoints = [this, channel]() {
        this->channelPointSignal_ =
            channel->channelPointsChanged.connect([this, channel]() {
                this->updateChannelPointsDisplay(channel);
            });
    };
    auto disconnectPoints = [this]() {
        this->channelPointSignal_ = pajlada::Signals::ScopedConnection();
    };

    connectPoints();

    this->focusLostPointsConnection_ =
        this->split_->focusLost.connect(disconnectPoints);
    this->focusedPointsConnection_ =
        this->split_->focused.connect([this, channel, connectPoints] {
            connectPoints();
            this->updateChannelPointsDisplay(channel);
            this->split_->scheduleDeferredTwitchRefresh(true);
        });

    updateActionButtons();
    QTimer::singleShot(0, this, [this] {
        this->updateActionRowCompactness();
    });
}

void SplitInput::clearChannelPointsDisplay()
{
    this->ui_.channelPointsLabel->clear();
    this->ui_.channelPointsLabel->setToolTip(channelPointsIdleToolTip());
    this->channelPointsLabelWanted_ = false;
    this->channelPointsManualRefreshLoading_ = false;
    this->updateActionRowCompactness();
}

void SplitInput::updateChannelPointsDisplay(TwitchChannel *channel)
{
    if (!channel || !channel->shouldShowChannelPoints())
    {
        this->clearChannelPointsDisplay();
        return;
    }

    if (channel->isChannelPointsFetchInFlight())
    {
        if (this->channelPointsManualRefreshLoading_ ||
            this->ui_.channelPointsLabel->text().isEmpty())
        {
            this->ui_.channelPointsLabel->setText("...");
            this->updateActionRowCompactness();
        }

        const auto tooltip = QStringLiteral("Channel Points: refreshing...");
        if (this->ui_.channelPointsLabel->toolTip() != tooltip)
        {
            this->ui_.channelPointsLabel->setToolTip(tooltip);
        }
        return;
    }

    this->channelPointsManualRefreshLoading_ = false;

    const auto balance = channel->channelPointBalance();
    const auto &error = channel->lastChannelPointsError();
    const auto text = (balance < 0 && !error.isEmpty())
                          ? QStringLiteral("--")
                          : formatChannelPointsValue(balance);
    auto tooltip = (balance < 0 && !error.isEmpty())
                       ? QStringLiteral("Channel Points unavailable")
                       : formatChannelPointsToolTip(balance);
    if (!error.isEmpty())
    {
        tooltip += "\nError: " + error;
    }

    if (this->ui_.channelPointsLabel->text() != text)
    {
        this->ui_.channelPointsLabel->setText(text);
        this->updateActionRowCompactness();
    }

    if (this->ui_.channelPointsLabel->toolTip() != tooltip)
    {
        this->ui_.channelPointsLabel->setToolTip(tooltip);
    }
}

void SplitInput::updateFonts()
{
    auto *app = getApp();
    this->ui_.textEdit->setFont(
        app->getFonts()->getFont(FontStyle::ChatMedium, this->scale()));

    auto channelPointsFont =
        app->getFonts()->getFont(FontStyle::UiMedium, this->scale());
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    channelPointsFont.setFeature(QFont::Tag("tnum"), 1);
#endif
    this->ui_.textEditLength->setFont(channelPointsFont);
    this->ui_.channelPointsLabel->setFont(channelPointsFont);
    this->ui_.sendWaitStatus->setFont(channelPointsFont);
#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
    this->ui_.channelPointRewardPromptTitle->setFont(channelPointsFont);
#endif

    this->ui_.replyLabel->setFont(
        app->getFonts()->getFont(FontStyle::ChatMediumBold, this->scale()));
    this->ui_.raidStatusLabel->setContentsMargins(
        int(std::round(6 * this->scale())), int(std::round(1 * this->scale())),
        int(std::round(6 * this->scale())),
        std::max(3, int(std::round(3 * this->scale()))));
    this->ui_.raidStatusLabel->setFont(
        app->getFonts()->getFont(FontStyle::UiMedium, this->scale()));
    this->ui_.raidStatusProgress->setFixedHeight(
        std::max(2, int(2 * this->scale())));
    this->ui_.nukePreviewLabel->setFont(
        app->getFonts()->getFont(FontStyle::UiMedium, this->scale()));
    if (this->ui_.translationPreviewLabel != nullptr)
    {
        this->ui_.translationPreviewLabel->setFont(
            app->getFonts()->getFont(FontStyle::UiMedium, this->scale()));
        this->ui_.translationPreviewLabel->setContentsMargins(
            int(std::round(6 * this->scale())), 0,
            int(std::round(6 * this->scale())), 0);
    }
    auto commandCompletionFont =
        app->getFonts()->getFont(FontStyle::UiMedium, this->scale());
    const auto commandRowHeight =
        std::max(18, int(std::round(20 * this->scale())));
    for (auto *row : this->ui_.commandCompletionRows)
    {
        if (row == nullptr)
        {
            continue;
        }

        row->setFont(commandCompletionFont);
        row->setFixedHeight(commandRowHeight);
        row->setContentsMargins(int(std::round(6 * this->scale())), 0,
                                int(std::round(6 * this->scale())), 0);
    }
    this->updateActionRowCompactness();
}

void SplitInput::setSendWaitStatus(const QString &text)
{
    this->ui_.sendWaitStatus->setText(text);
    this->sendWaitStatusWanted_ =
        !text.isEmpty() && getSettings()->showSendWaitTimer;
    this->updateActionRowCompactness();
}

void SplitInput::updateChannel()
{
    this->channelConnections_.clear();

    auto refreshSelectedChannelState = [this] {
        auto selected = this->split_->getSelectedChannel();
        if (selected == nullptr)
        {
            return;
        }

        this->ui_.textEdit->setCompleter(
            new QCompleter(selected->completionModel));
        this->inputHighlighter->setChannel(selected);
        this->checkSpellingChanged();
        this->bindNukePreviewChannel();
        this->updateNukePreview(this->ui_.textEdit->toPlainText());
        this->bindRaidStatusChannel();
        this->updateRaidStatus();
        this->resetCommandCompletionSession();
        this->clearOutgoingTranslationPreview();
        this->updateOutgoingTranslationButton();
        this->bindChannelPoints(dynamic_cast<TwitchChannel *>(selected.get()));
    };

    auto channel = this->split_->getChannel();
    if (auto *multiChannel = dynamic_cast<MultiChannel *>(channel.get()))
    {
        this->channelConnections_.managedConnect(
            multiChannel->activeChannelChanged, [this] {
                this->updateChannel();
            });
    }

    refreshSelectedChannelState();
}

}  // namespace chatterino

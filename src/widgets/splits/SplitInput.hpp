// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Message.hpp"
#include "providers/moltorino/MoltorinoFeatureFlags.hpp"
#include "widgets/BaseWidget.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPaintEvent>
#include <QPointer>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
#    include <functional>
#endif
#include <array>
#include <memory>
#include <vector>

namespace chatterino {

class Split;
class EmotePopup;
class InputCompletionPopup;
class InputHighlighter;
class MessageView;
class LabelButton;
class ResizingTextEdit;
class ChannelView;
class SvgButton;
class SpellCheckHighlighter;
class Channel;
using ChannelPtr = std::shared_ptr<Channel>;
class TwitchChannel;
enum class CompletionKind;

class SplitInput : public BaseWidget
{
    Q_OBJECT

public:
    SplitInput(Split *_chatWidget, bool enableInlineReplying = true);
    SplitInput(QWidget *parent, Split *_chatWidget, ChannelView *_channelView,
               bool enableInlineReplying = true);

    bool hasSelection() const;
    void clearSelection() const;

    bool isEditFirstWord() const;
    QString getInputText() const;
    void insertText(const QString &text);

    void setReply(MessagePtr target, std::weak_ptr<Channel> channel);
    void setPlaceholderText(const QString &text);

    /**
     * @brief Hide the widget
     *
     * This is a no-op if the SplitInput is already hidden
     **/
    void hide();

    /**
     * @brief Show the widget
     *
     * This is a no-op if the SplitInput is already shown
     **/
    void show();

    /**
     * @brief Returns the hidden or shown state of the SplitInput
     *
     * Hidden in this context means "has 0 height", meaning it won't be visible
     * but Qt still treats the widget as "technically visible" so we receive events
     * as if the widget is visible
     **/
    bool isHidden() const;

    /**
     * @brief Sets the text of this input
     *
     * This method should only be used in tests
     */
    void setInputText(const QString &newInputText);

    /**
     * @brief Sets a formatted time to sendWaitStatus
     *
     * This method is used to update the text of the timeout and slow mode timer
     */
    void setSendWaitStatus(const QString &text);

    void triggerSelfMessageReceived();

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
    void showChannelPointRewardPrompt(
        const QString &title, const QString &placeholder, bool requireText,
        std::function<void(const QString &)> submitCallback);
    void hideChannelPointRewardPrompt();
#endif

    std::optional<bool> checkSpellingOverride() const;
    void setCheckSpellingOverride(std::optional<bool> override);

    pajlada::Signals::Signal<const QString &> textChanged;
    pajlada::Signals::NoArgSignal selectionChanged;

protected:
    QSize minimumSizeHint() const override;

    void scaleChangedEvent(float scale_) override;
    void themeChangedEvent() override;

    void paintEvent(QPaintEvent * /*event*/) override;
    void resizeEvent(QResizeEvent * /*event*/) override;

    void mousePressEvent(QMouseEvent *event) override;

    virtual void giveFocus(Qt::FocusReason reason);

    QString handleSendMessage(const std::vector<QString> &arguments);
    void postMessageSend(const QString &message,
                         const std::vector<QString> &arguments);

    /// Clears the input box, clears reply target if inline replies are enabled
    void clearInput();

    void addShortcuts() override;
    void initLayout();
    bool eventFilter(QObject *obj, QEvent *event) override;
    void installTextEditEvents();
    void onCursorPositionChanged();
    void onTextChanged();
    void bindNukePreviewChannel();
    void scheduleNukePreviewRefresh();
    void updateNukePreview(const QString &text);
    void applyNukePreview();
    void bindRaidStatusChannel();
    void updateRaidStatus();
    bool trySendMessageAsWarning(const QString &message,
                                 const ChannelPtr &channel);
    bool maybeSendMessageAsWarning(const QString &message,
                                   const std::vector<QString> &arguments,
                                   const ChannelPtr &channel);
    void updateEmoteButton();
    void bindChannelPoints(TwitchChannel *channel);
    void clearChannelPointsDisplay();
    void updateChannelPointsDisplay(TwitchChannel *channel);
    void updateActionRowCompactness();
    void updateCompletionPopup();
    void updateOutgoingTranslationPreview();
    void scheduleOutgoingTranslationPreview(const QString &text);
    void applyOutgoingTranslationPreview();
    void clearOutgoingTranslationPreview();
    void updateOutgoingTranslationButton();
    void openOutgoingTranslationMenu();
    QString outgoingTranslationChannelName() const;
    QString outgoingTranslationMode() const;
    QString outgoingTranslationTargetLanguage() const;
    bool shouldTranslateOutgoingMessage(const QString &message) const;
    QString currentOutgoingMessageBody() const;
    bool maybeSendTranslatedMessage(const QString &message,
                                    const std::vector<QString> &arguments,
                                    const ChannelPtr &channel);
    void postTranslatedMessageSend(const QString &message,
                                   const std::vector<QString> &arguments);
    bool updateCommandCompletion(const QString &query, int start, int end);
    void renderCommandCompletion();
    void hideCommandCompletion();
    bool moveCommandCompletionSelection(int offset);
    void showCompletionPopup(const QString &text, CompletionKind kind);
    void hideCompletionPopup();
    void insertCompletionText(const QString &input_);
    bool handleCommandCompletionKey(QKeyEvent *event);
    void insertCommandCompletionText(const QString &completion, bool keepPopup);
    void resetCommandCompletionSession();
    void openEmotePopup();
    void clearReplyTarget();
#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
    bool submitChannelPointRewardPrompt();
#endif

    void updateCancelReplyButton();

    // scaledMaxHeight returns the height in pixels that this widget can grow to
    // This does not take hidden into account, so callers must take hidden into account themselves
    int scaledMaxHeight() const;

    // Returns true if the channel this input is connected to is a Twitch channel,
    // the user's setting is set to Prevent, and the given text goes beyond the Twitch message length limit
    bool shouldPreventInput(const QString &text) const;

    int marginForTheme() const;

    void applyOuterMargin();
    void relayoutParentWidgets();

    int replyMessageWidth() const;

    Split *const split_;
    ChannelView *const channelView_;
    QPointer<EmotePopup> emotePopup_;
    QPointer<InputCompletionPopup> inputCompletionPopup_;

    struct {
        // vbox for all components
        QVBoxLayout *vbox;

        // reply widgets
        QWidget *replyWrapper;
        QVBoxLayout *replyVbox;
        QHBoxLayout *replyHbox;
        MessageView *replyMessage;
        QLabel *replyLabel;
        SvgButton *cancelReplyButton;
        QWidget *raidStatusWidget;
        QVBoxLayout *raidStatusLayout;
        QLabel *raidStatusLabel;
        QProgressBar *raidStatusProgress;
        QLabel *nukePreviewLabel;
        QWidget *commandCompletionWidget;
        QVBoxLayout *commandCompletionLayout;
        std::array<QLabel *, 3> commandCompletionRows{};
        QWidget *translationPreviewWidget;
        QLabel *translationPreviewLabel;
#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
        QWidget *channelPointRewardPromptWidget;
        QLabel *channelPointRewardPromptTitle;
#endif

        // input widgets
        QWidget *inputWrapper;
        QHBoxLayout *inputHbox;
        ResizingTextEdit *textEdit;
        QLabel *textEditLength;
        LabelButton *sendButton;
        QLabel *sendWaitStatus;
        QHBoxLayout *buttonsRow;
        QLabel *channelPointsLabel;
        SvgButton *predictionButton;
        SvgButton *pollButton;
        SvgButton *outgoingTranslateButton;
        SvgButton *emoteButton;
    } ui_{};

    MessagePtr replyTarget_ = nullptr;
    std::weak_ptr<Channel> replyChannel_;
    bool enableInlineReplying_;

    pajlada::Signals::SignalHolder managedConnections_;
    pajlada::Signals::SignalHolder channelConnections_;
    pajlada::Signals::ScopedConnection channelPointSignal_;
    pajlada::Signals::ScopedConnection modStateSignal_;
    pajlada::Signals::ScopedConnection pollStateSignal_;
    pajlada::Signals::ScopedConnection focusedPointsConnection_;
    pajlada::Signals::ScopedConnection focusLostPointsConnection_;
    QStringList prevMsg_;
    QString currMsg_;
    int prevIndex_ = 0;
    struct CommandCompletionSession {
        bool active = false;
        bool inserted = false;
        bool selectionChanged = false;
        QString query;
        int start = 0;
        int end = 0;
    } commandCompletionSession_;
    struct CommandCompletionSuggestion {
        QString completion;
        QString usage;
    };
    std::vector<CommandCompletionSuggestion> commandCompletionSuggestions_;
    int commandCompletionSelectedIndex_ = 0;
    bool updatingCommandCompletionText_ = false;
    QTimer nukePreviewTimer_;
    QTimer outgoingTranslationPreviewTimer_;
    QString pendingNukePreviewText_;
    QString pendingOutgoingTranslationText_;
    QString outgoingTranslationPreviewSource_;
    QString outgoingTranslationPreviewTarget_;
    QString outgoingTranslationPreviewText_;
    int outgoingTranslationGeneration_ = 0;
    bool outgoingTranslationSendInFlight_ = false;
    bool nukePreviewCommandActive_ = false;
    pajlada::Signals::ScopedConnection nukePreviewMessageConnection_;
    pajlada::Signals::ScopedConnection nukePreviewReplaceConnection_;
    pajlada::Signals::ScopedConnection nukePreviewClearConnection_;
    QTimer raidStatusTimer_;
    QPropertyAnimation raidStatusProgressAnimation_;
    pajlada::Signals::ScopedConnection raidStatusConnection_;
    bool sendWaitStatusWanted_ = false;
    bool channelPointsLabelWanted_ = false;
    bool channelPointsManualRefreshLoading_ = false;
    qint64 lastManualChannelPointsRefreshMs_ = 0;
    bool predictionButtonWanted_ = false;
    bool pollButtonWanted_ = false;
#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
    bool channelPointRewardPromptRequiresText_ = false;
    std::function<void(const QString &)> channelPointRewardPromptSubmit_;
#endif

    // Hidden denotes whether this split input should be hidden or not
    // This is used instead of the regular QWidget::hide/show because
    // focus events don't work as expected, so instead we use this bool and
    // set the height of the split input to 0 if we're supposed to be hidden instead
    bool hidden{false};

    /// Updates the text edit palette using the current theme
    /// and current "backgroundColor" property
    void updateTextEditPalette();

    // the background color defines the current background color of this split input
    // instead of reading straight from the theme, we store a property here
    // to be used by a property to be able to pulse a highlight color on demand
    Q_PROPERTY(
        QColor backgroundColor READ backgroundColor WRITE setBackgroundColor);

    QColor backgroundColor_{"#000000"};
    QColor backgroundColor() const;
    void setBackgroundColor(QColor newColor);

    QPropertyAnimation backgroundColorAnimation;

    std::optional<bool> checkSpellingOverride_;
    bool shouldCheckSpelling() const;
    void checkSpellingChanged();

    InputHighlighter *inputHighlighter = nullptr;

    void updateFonts();

private Q_SLOTS:
    void editTextChanged();

    void updateChannel();

    friend class Split;
    friend class ReplyThreadPopup;
};

}  // namespace chatterino

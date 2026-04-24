// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "singletons/Paths.hpp"
#include "widgets/BaseWindow.hpp"
#include "widgets/DraggablePopup.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QMovie>
#include <QPixmap>
#include <QPointer>

#include <chrono>
#include <functional>
#include <map>

class QCheckBox;
class QKeyEvent;
class QMovie;

namespace chatterino {

class Channel;
using ChannelPtr = std::shared_ptr<Channel>;
class Label;
class MarkdownLabel;
class EditUserNotesDialog;
class ChannelView;
class Split;
struct HelixUser;
class LabelButton;
class PixmapButton;
class ColorSwatch;
class LiveIndicator;

class UserInfoPopup final : public DraggablePopup
{
    Q_OBJECT

public:
    /**
     * @param closeAutomatically Decides whether the popup should close when it loses focus
     * @param split Will be used as the popup's parent. Must not be null
     */
    UserInfoPopup(bool closeAutomatically, Split *split);

    void setData(const QString &name, const ChannelPtr &channel);
    void setData(const QString &name, const ChannelPtr &contextChannel,
                 const ChannelPtr &openingChannel);

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    void windowDeactivationEvent() override;

    void keyPressEvent(QKeyEvent *event) override;

private:
    /// Registers a mnemonic from the button's label (e.g. "&Logs" -> Alt+L).
    /// When Alt+key is pressed and the button is visible, @a action is invoked.
    void registerMnemonicButton(LabelButton *button,
                                std::function<void()> action);

    void installEvents();
    void updateUserData();
    void updateLatestMessages();
    void updateNotes();

    void loadAvatar(const QString &userID, const QString &pictureURL,
                    bool isKick);

    void loadSevenTVAvatar(const QString &userID, bool isKick);
    void setSevenTVAvatar(const QString &filename, const QByteArray &format);

    void saveCacheAvatar(const QByteArray &avatar,
                         const QString &filename) const;

    void updateAvatarUrl();

    void updateKickUserData();
    void onKickProfilePictureClick(Qt::MouseButton button);

    /// Shows the profile picture context menu (avatar right-click menu).
    /// Returns an error message if the menu could not be shown, empty on success.
    QString showProfilePictureContextMenu();

    QStringView platformName() const;

    void appendCommonProfileActions(QMenu *menu);

    bool isMod_{};
    bool isBroadcaster_{};

    Split *split_;

    QString userName_;
    QString userId_;
    QString avatarUrl_;
    QString helixAvatarUrl_;
    QString seventvAvatarUrl_;
    QString seventvUserID_;

    QString kickUserSlug_;

    // The channel the popup was opened from (e.g. /mentions or #forsen). Can be a special channel.
    ChannelPtr channel_;

    // The channel the messages are rendered from (e.g. #forsen). Can be a special channel, but will try to not be where possible.
    ChannelPtr underlyingChannel_;

    pajlada::Signals::NoArgSignal userStateChanged_;

    std::unique_ptr<pajlada::Signals::ScopedConnection> refreshConnection_;
    std::unique_ptr<pajlada::Signals::ScopedConnection>
        userDataUpdatedConnection_;

    // If we should close the dialog automatically if the user clicks out
    // Set based on the "Automatically close usercard when it loses focus" setting
    // Pinned status is tracked in DraggablePopup::isPinned_.
    const bool closeAutomatically_;

    pajlada::Signals::SignalHolder signalHolder_;

    class TimeoutWidget;
    struct {
        PixmapButton *avatarButton = nullptr;
        PixmapButton *localizedNameCopyButton = nullptr;

        Label *nameLabel = nullptr;
        Label *localizedNameLabel = nullptr;
        Label *pronounsLabel = nullptr;
        Label *followerCountLabel = nullptr;
        Label *createdDateLabel = nullptr;
        Label *lastLiveLabel = nullptr;
        ColorSwatch *colorSwatch = nullptr;
        Label *colorLabel = nullptr;
        Label *chattersLabel = nullptr;
        Label *userIDLabel = nullptr;
        Label *followageLabel = nullptr;
        Label *subageLabel = nullptr;
        Label *rolesLabel = nullptr;

        LiveIndicator *liveIndicator = nullptr;

        QCheckBox *block = nullptr;
        QCheckBox *ignoreHighlights = nullptr;
        MarkdownLabel *notesPreview = nullptr;
        LabelButton *notesAdd = nullptr;

        Label *noMessagesLabel = nullptr;
        ChannelView *latestMessages = nullptr;

        LabelButton *usercardLabel = nullptr;
        LabelButton *switchAvatars = nullptr;
        LabelButton *userlogsLabel = nullptr;

        TimeoutWidget *timeoutWidget = nullptr;
    } ui_;

    QMovie *seventvAvatar_ = nullptr;
    bool isTwitchAvatarShown_ = true;
    QPixmap avatarPixmap_;
    QPointer<EditUserNotesDialog> editUserNotesDialog_;

    bool isKick_ = false;
    uint64_t kickUserID_ = 0;

    /// Alt+key -> (action, visibility check). Populated from button labels
    /// containing "&" (e.g. "&Logs"). Same logic as ChannelView menu mnemonics.
    std::map<int, std::pair<std::function<void()>, std::function<bool()>>>
        mnemonicActions_;

    class TimeoutWidget : public BaseWidget
    {
    public:
        enum Action { Ban, Unban, Timeout };

        TimeoutWidget();

        pajlada::Signals::Signal<std::pair<Action, int>> buttonClicked;

        void setMinTimeout(int minSecs);

    protected:
        void paintEvent(QPaintEvent *event) override;

    private:
        std::vector<std::pair<QWidget *, int>> timeoutButtons;
    };
};

}  // namespace chatterino

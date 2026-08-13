// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/twitch/TwitchNameHistory.hpp"
#include "singletons/Paths.hpp"
#include "widgets/BaseWindow.hpp"
#include "widgets/DraggablePopup.hpp"

#include <boost/signals2/connection.hpp>
#include <pajlada/signals/scoped-connection.hpp>
#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QDateTime>
#include <QMovie>
#include <QPixmap>
#include <QPointer>
#include <QString>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

class QCheckBox;
class QKeyEvent;
class QLabel;
class QMenu;
class QMovie;
class QWidget;

namespace chatterino {

class Channel;
using ChannelPtr = std::shared_ptr<Channel>;
struct Message;
using MessagePtr = std::shared_ptr<const Message>;
enum class MessagePlatform : uint8_t;
class Label;
class MarkdownLabel;
class EditUserNotesDialog;
class ChannelView;
class Split;
struct HelixUser;
struct IvrSubage;
struct IvrUserProfile;
class LabelButton;
class LiveIndicator;
class Paint;
class PixmapButton;
class SvgButton;

class UserInfoPopup final : public DraggablePopup
{
    Q_OBJECT

public:
    /**
     * @param closeAutomatically Decides whether the popup should close when it loses focus
     * @param split Will be used as the popup's parent. Must not be null
     */
    UserInfoPopup(bool closeAutomatically, Split *split);
    ~UserInfoPopup() override;

    void setData(const QString &name, const ChannelPtr &channel);
    void setData(const QString &name, const ChannelPtr &contextChannel,
                 const ChannelPtr &openingChannel);

    void setYouTubeContext();

    static void openUserChannelAction(const QString &userName,
                                      MessagePlatform platform,
                                      const QString &channelName,
                                      const QString &channelId);

    static void notifyFollowMutation(const QString &targetId,
                                     const QString &requestUserId,
                                     const QString &requestLogin,
                                     bool unfollow);

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    void windowDeactivationEvent() override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void registerMnemonicButton(LabelButton *button, int key,
                                std::function<void()> action);

    void installEvents();
    void updateUserData();
    void updateLatestMessages();
    void updateUsercardMessagesVisibility();
    void resetUsercardMessageLoader();
    void updateLoadMoreMessagesButton();
    bool canLoadMoreUsercardMessages() const;
    void maybeStartUsercardMessageAutoLoad();
    void requestMoreUsercardMessages(bool enableLazyLoadOnSuccess);
    void maybeLoadMoreUsercardMessagesFromScroll();
    void fetchMoreUsercardMessages(int emptyPageSkipsLeft,
                                   bool enableLazyLoadOnSuccess);
    void updateNotes();
    void refreshSevenTVUserButtonVisibility();
    void refreshSeventvPaint();
    void updateSeventvPaintPixmap();
    void resetNameHistory();
    bool applyCachedNameHistory();
    void updateNameHistoryButton();
    void updateBadgesButton();
    void openBadgesDialog();
    void showNameHistoryMenu();
    void openNameHistoryMenu(const QString &statusText = {});
    void requestNameHistory();
    void updateUsercardStatusIcons();
    void updateUsercardSubGiftRow(const IvrSubage &subageInfo);
    void hideUsercardSubGiftRow();
    void resetUsercardInfoRows();
    void applyIvrUserProfile(const IvrUserProfile &profile);
    void updateLiveIndicatorDisplay();

    bool isFollowing() const;
    bool isFollowingStatusKnown() const;
    void setFollowingStatus(bool following,
                            std::optional<QDateTime> followedAt = std::nullopt);
    void refreshFollowingStatus(bool force = false);
    void resetFollowingStatus();
    void updateUsercardFollowButton();
    void toggleUsercardFollow();

    void loadAvatar(const QString &userID, const QString &pictureURL,
                    bool isKick);

    void loadSevenTVAvatar(const QString &userID, bool isKick,
                           bool allowAvatarDownload = true);
    void setSevenTVAvatar(const QString &filename, const QByteArray &format);

    void saveCacheAvatar(const QByteArray &avatar,
                         const QString &filename) const;

    void updateAvatarUrl();

    void updateKickUserData();
    void updateYouTubeUserData();
    void updateYouTubeLatestMessages();
    void loadYouTubeAvatar(const QString &url);
    void onKickProfilePictureClick(Qt::MouseButton button);
    bool canShowRoleManagementMenu() const;
    void showRoleManagementMenu();
    void runRoleManagementCommand(const QString &command,
                                  const QString &actionText);

    /// Shows the profile picture context menu (avatar right-click menu).
    /// Returns an error message if the menu could not be shown, empty on success.
    QString showProfilePictureContextMenu();

    QStringView platformName() const;

    void appendCommonProfileActions(QMenu *menu);
    void refreshTargetModerationStatus();
    bool updateTargetModerationStatusFromMessage(const MessagePtr &message);
    bool shouldShowModerationActions() const;

    enum class UsercardModerationAction { Ban, Unban, Timeout };

    struct UsercardModerationRequest {
        UsercardModerationAction action{};
        int durationSeconds = -1;
        QString reason;
        bool promptForReason = false;
    };

    void executeUsercardModerationAction(
        const UsercardModerationRequest &request);
    void showUsercardModerationReasonPopup(
        const UsercardModerationRequest &request);

    bool isMod_{};
    bool isBroadcaster_{};

    Split *split_;

    QString userName_;
    QString userId_;
    uint64_t userDataRequestGeneration_ = 0;
    QString avatarUrl_;
    QString helixAvatarUrl_;
    QString seventvAvatarUrl_;
    QString seventvUserID_;
    int seventvUserRequestGeneration_ = 0;
    bool seventvUserLookupInFlight_ = false;
    bool seventvUserLookupFinished_ = false;
    QString nameHistoryLogin_;
    std::vector<TwitchNameHistoryEntry> nameHistoryEntries_;
    QPointer<QMenu> nameHistoryMenu_;
    uint64_t nameHistoryRequestGeneration_ = 0;
    bool nameHistoryLoading_ = false;
    bool nameHistoryLoaded_ = false;
    ChannelPtr usercardMessagesChannel_;
    QString usercardMessagesCursor_;
    QString usercardMessagesError_;
    uint64_t usercardMessagesRequestGeneration_ = 0;
    bool usercardMessagesLoading_ = false;
    bool usercardMessagesHasNextPage_ = true;
    bool usercardMessagesLazyLoadEnabled_ = false;

    QString kickUserSlug_;
    QPointer<QWidget> moderationReasonPopup_;

    // The channel the popup was opened from (e.g. /mentions or #forsen). Can be a special channel.
    ChannelPtr channel_;

    // The channel the messages are rendered from (e.g. #forsen). Can be a special channel, but will try to not be where possible.
    ChannelPtr underlyingChannel_;

    pajlada::Signals::NoArgSignal userStateChanged_;
    pajlada::Signals::NoArgSignal followingStatusChanged_;

    bool following_ = false;
    bool followingStatusKnown_ = false;
    std::optional<QDateTime> followedAt_;
    QString followingStatusUserId_;
    QDateTime lastFollowingStatusRefreshAt_;
    std::atomic<bool> followingStatusFetchInFlight_{false};
    std::unique_ptr<pajlada::Signals::ScopedConnection>
        followingStatusChangedConnection_;

    std::unique_ptr<pajlada::Signals::ScopedConnection> refreshConnection_;
    std::unique_ptr<pajlada::Signals::ScopedConnection>
        twitchUserStateConnection_;
    std::unique_ptr<pajlada::Signals::ScopedConnection>
        userDataUpdatedConnection_;
    std::unique_ptr<pajlada::Signals::ScopedConnection>
        usercardScrollConnection_;
    boost::signals2::scoped_connection currentUserChangedConnection_;

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
        SvgButton *followButton = nullptr;
        LabelButton *nameHistoryButton = nullptr;
        Label *localizedNameLabel = nullptr;
        Label *pronounsLabel = nullptr;
        Label *followerCountLabel = nullptr;
        Label *createdDateLabel = nullptr;
        Label *lastLiveLabel = nullptr;
        Label *userIDLabel = nullptr;
        QLabel *bannedAvatarLabel = nullptr;
        QWidget *followageRow = nullptr;
        QLabel *followageIcon = nullptr;
        Label *followageLabel = nullptr;
        QWidget *subageRow = nullptr;
        QLabel *subageIcon = nullptr;
        Label *subageLabel = nullptr;
        QWidget *subGiftRow = nullptr;
        QLabel *subGiftIcon = nullptr;
        Label *subGiftLabel = nullptr;
        Label *chatterCountLabel = nullptr;
        QWidget *userColorRow = nullptr;
        QWidget *userColorSwatch = nullptr;
        Label *userColorLabel = nullptr;
        Label *statusLabel = nullptr;

        LiveIndicator *liveIndicator = nullptr;

        QCheckBox *block = nullptr;
        QCheckBox *ignoreHighlights = nullptr;
        MarkdownLabel *notesPreview = nullptr;
        LabelButton *notesAdd = nullptr;

        Label *noMessagesLabel = nullptr;
        ChannelView *latestMessages = nullptr;
        LabelButton *loadMoreMessages = nullptr;

        LabelButton *usercardLabel = nullptr;
        LabelButton *userlogsLabel = nullptr;
        LabelButton *badgesLabel = nullptr;
        LabelButton *sevenTVUserLabel = nullptr;
        QLabel *seventvPaintPixmapLabel = nullptr;
        QWidget *seventvPaintRow = nullptr;
        LabelButton *rolesLabel = nullptr;
        LabelButton *switchAvatars = nullptr;

        TimeoutWidget *timeoutWidget = nullptr;
    } ui_;

    QMovie *seventvAvatar_ = nullptr;
    std::shared_ptr<Paint> seventvPaint_;
    bool isTwitchAvatarShown_ = true;
    QPixmap avatarPixmap_;
    QPointer<EditUserNotesDialog> editUserNotesDialog_;

    bool isKick_ = false;
    bool isYouTube_ = false;
    bool forceYouTube_ = false;
    QString youtubeChannelId_;
    uint64_t kickUserID_ = 0;
    bool isUserLive_ = false;
    int liveViewerCount_ = 0;

    std::map<int, std::pair<std::function<void()>, std::function<bool()>>>
        mnemonicActions_;

    class TimeoutWidget : public BaseWidget
    {
    public:
        TimeoutWidget();

        pajlada::Signals::Signal<UsercardModerationRequest> buttonClicked;

        void setMinTimeout(int minSecs);

    protected:
        void paintEvent(QPaintEvent *event) override;

    private:
        std::vector<std::pair<QWidget *, int>> timeoutButtons;
    };
};

}  // namespace chatterino

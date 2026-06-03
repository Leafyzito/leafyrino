#pragma once

#include "providers/moltorino/MoltorinoFeatureFlags.hpp"

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS

#    include "providers/twitch/api/TwitchGql.hpp"
#    include "widgets/DraggablePopup.hpp"

#    include <pajlada/signals/scoped-connection.hpp>
#    include <QPointer>
#    include <QString>
#    include <QTimer>
#    include <QVector>

#    include <vector>

class QButtonGroup;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QResizeEvent;
class QShowEvent;
class QVBoxLayout;

namespace chatterino {

class Button;
class SplitInput;
class SvgButton;
class TwitchChannel;

class ChannelPointsDialog : public DraggablePopup
{
public:
    ChannelPointsDialog(TwitchChannel *channel, SplitInput *input,
                        QWidget *parent = nullptr);

    static void showDialog(TwitchChannel *channel, SplitInput *input,
                           QWidget *parent = nullptr);

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    enum class View {
        Rewards,
        RewardDetail,
        Emotes,
    };

    void refreshHeader();
    void refreshStyle();
    void reloadRewards(bool force);
    void rebuildContent();
    void refreshCurrentLayout();
    void refreshRewardsLayout();
    void refreshRewardDetailLayout();
    void refreshEmotesLayout();
    void rebuildRewards();
    void rebuildRewardDetail();
    void rebuildEmotes();
    void clearContent();
    void setStatus(const QString &text, bool error = false);
    void showRewardsView();
    void openRewardDetail(const GqlChannelPointReward &reward);
    void activateSelectedReward();
    void selectReward(const GqlChannelPointReward &reward);
    void redeemCustomReward(const GqlChannelPointReward &reward,
                            const QString &prompt);
    void unlockRandomEmote(const GqlChannelPointReward &reward);
    void openEmotePicker(const GqlChannelPointReward &reward, bool modified);
    void loadEmotePickerData();
    void unlockSelectedEmote(const GqlChannelPointEmote &emote);
    void applyRedeemResult(const GqlChannelPointRedeemResult &result,
                           const QString &message);
    QString redeemChannelId() const;
    void queueEmoteImageRefresh();
    void queueRewardImageRefresh();
    void queueEmoteLazyLoad();
    void scheduleLayoutRefresh(int delayMs = 35);
    QString authTokenOrMessage();
    bool canRedeem(const GqlChannelPointReward &reward);
    void applySizeConstraints();

    TwitchChannel *channel_{};
    QPointer<SplitInput> input_;

    QVBoxLayout *mainLayout_{};
    QWidget *headerWidget_{};
    QPushButton *backButton_{};
    QLabel *headerTitleLabel_{};
    QLabel *headerSubtitleLabel_{};
    Button *pinButton_{};
    SvgButton *closeButton_{};
    QScrollArea *scrollArea_{};
    QWidget *contentWidget_{};
    QVBoxLayout *contentLayout_{};
    QLabel *statusLabel_{};
    QLineEdit *emoteSearchInput_{};

    View view_ = View::Rewards;
    QString rewardsChannelId_;
    QVector<GqlChannelPointReward> rewards_;
    QVector<GqlChannelPointEmote> emotes_;
    QVector<GqlChannelPointEmoteModifier> modifiers_;
    GqlChannelPointReward selectedReward_;
    bool selectedRewardValid_ = false;
    bool selectingModifiedEmote_ = false;
    bool emotesLoadedForModifiedPicker_ = false;
    int emoteVisibleLimit_ = 48;
    int emoteScrollValue_ = 0;
    int emoteImageRefreshAttempts_ = 0;
    bool emoteImageRefreshQueued_ = false;
    int rewardImageRefreshAttempts_ = 0;
    bool rewardImageRefreshQueued_ = false;
    bool emoteLazyLoadQueued_ = false;
    QString selectedModifierId_;
    QString emoteSearch_;
    bool rewardsLoading_ = false;
    bool emotesLoading_ = false;
    bool actionInFlight_ = false;
    bool rebuildingContent_ = false;
    bool refreshingLayout_ = false;
    bool rebuildQueued_ = false;
    bool layoutRefreshQueued_ = false;
    bool initialFetchDone_ = false;
    QString statusText_;
    bool statusIsError_ = false;
    QTimer layoutRefreshTimer_;

    pajlada::Signals::ScopedConnection channelPointsConnection_;

    static std::vector<QPointer<ChannelPointsDialog>> activeDialogs_;
};

}  // namespace chatterino

#endif

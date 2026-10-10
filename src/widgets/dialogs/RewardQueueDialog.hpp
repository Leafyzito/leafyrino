#pragma once

#include "providers/twitch/api/TwitchGql.hpp"
#include "widgets/DraggablePopup.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QDateTime>
#include <QHash>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

class QCheckBox;
class QJsonObject;
class QLabel;
class QPushButton;
class QScrollArea;
class QShowEvent;
class QVBoxLayout;

namespace chatterino {

class TwitchChannel;

class RewardQueueDialog : public DraggablePopup
{
public:
    RewardQueueDialog(TwitchChannel *channel, QWidget *parent = nullptr);

    static void showDialog(TwitchChannel *channel, QWidget *parent = nullptr);

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    void showEvent(QShowEvent *event) override;

private:
    void refreshStyle();
    void applySizeConstraints();
    void reload(bool quiet = false);
    void refreshCounts();
    void applyRewardQueue(GqlRewardQueue queue);
    void loadRedemptions(bool append);
    void loadUsers(const QStringList &userIds, std::function<void()> callback);
    void finishLoading();
    void onRewardRedeemed(const QJsonObject &redemption);
    void onRedemptionStatusUpdated(const QJsonObject &redemption);
    void onBulkUpdateProgress(const QJsonObject &progress, bool finished);
    void resolveLocally(const QString &redemptionId, const QString &rewardId,
                        bool adjustCount = true);
    bool isNewerThanSnapshot(const QDateTime &timestamp) const;
    void scheduleUnpinParentOnClose(QWidget *parent);
    const GqlRewardRedemption *findRedemption(
        const QString &redemptionId) const;
    void setIdleStatus();
    void onTick();
    void rebuildSidebar(bool force = false);
    void setRewardPaused(const QString &rewardId, bool paused);
    void togglePaused(bool paused);
    void rebuildList();
    QWidget *createRow(const GqlRewardRedemption &redemption);
    void updateActions();
    void selectReward(const QString &rewardId);
    void toggleSortOrder();
    void updateSortButton();
    void updateRedemptions(const QStringList &ids, bool fulfill);
    void sendStatusUpdate(const QStringList &ids, bool fulfill,
                          const QString &token);
    void finishStatusUpdate(const QString &error);
    void updateAll(bool fulfill);
    void queueReload();
    void setStatus(const QString &text, bool error = false);
    QString authTokenOrMessage(const QString &action);
    QStringList loadedIds() const;
    QStringList selectedIds() const;
    void loadPixmap(const QString &url,
                    std::function<void(const QPixmap &)> callback);

    QString channelLogin_;
    QString channelId_;

    QVector<GqlRewardQueueReward> rewards_;
    QVector<GqlRewardRedemption> redemptions_;
    QHash<QString, GqlRewardQueueUser> users_;
    QStringList rewardOrder_;
    QStringList sidebarIds_;
    QSet<QString> selected_;
    QSet<QString> resolvedIds_;
    QSet<QString> redeemedIds_;
    QSet<QString> badgeFetchFailed_;
    QStringList inFlightIds_;
    QString pendingError_;
    QString selectedRewardId_;
    QString nextCursor_;
    bool hasNextPage_ = false;
    bool newestFirst_ = true;
    bool extraPagesLoaded_ = false;
    bool loading_ = false;
    bool pauseInFlight_ = false;
    bool quietLoad_ = false;
    bool statusIsError_ = false;
    bool actionInFlight_ = false;
    bool countsRefreshInFlight_ = false;
    bool parentUnpinScheduled_ = false;
    std::optional<bool> updateAllStatus_;
    int updateAllRounds_ = 0;
    int generation_ = 0;
    QDateTime countsValidAt_;

    QVBoxLayout *sidebarLayout_{};
    QScrollArea *sidebarScrollArea_{};
    QVBoxLayout *listLayout_{};
    QScrollArea *listScrollArea_{};
    QLabel *titleLabel_{};
    QLabel *statusLabel_{};
    QCheckBox *selectAllCheckBox_{};
    QCheckBox *pauseCheckBox_{};
    QHash<QString, QPushButton *> sidebarButtons_;
    QPushButton *completeSelectedButton_{};
    QPushButton *rejectSelectedButton_{};
    QPushButton *completeAllButton_{};
    QPushButton *rejectAllButton_{};
    QPushButton *loadMoreButton_{};
    QPushButton *sortButton_{};
    QHash<QString, QCheckBox *> rowCheckBoxes_;
    std::vector<std::pair<QPointer<QLabel>, QDateTime>> timeLabels_;
    QTimer reloadTimer_;
    QTimer tickTimer_;

    QHash<QString, QPixmap> pixmaps_;
    QHash<QString, std::vector<std::function<void(const QPixmap &)>>>
        pendingPixmaps_;

    std::vector<pajlada::Signals::ScopedConnection> managedConnections_;

    static std::vector<QPointer<RewardQueueDialog>> activeDialogs_;
};

}  // namespace chatterino

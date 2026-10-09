#pragma once

#include "ForwardDecl.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QDialog>
#include <QHash>
#include <QPointer>
#include <QVector>

#include <memory>

class QCloseEvent;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace chatterino {

enum class CrossChannelAction {
    Ban,
    Unban,
};

class CrossBanDialog final : public QDialog
{
public:
    CrossBanDialog(CrossChannelAction action, QString targetId,
                   QString targetLogin, QString targetDisplayName,
                   QString actingLogin, QString oauthToken,
                   QVector<MoltorinoAuthChannel> channels,
                   QString currentChannelId,
                   std::weak_ptr<Channel> outputChannel,
                   QWidget *parent = nullptr);

    static void showDialog(CrossChannelAction action, const QString &targetId,
                           const QString &targetLogin,
                           const QString &targetDisplayName,
                           const QString &actingLogin,
                           const QString &oauthToken,
                           const QVector<MoltorinoAuthChannel> &channels,
                           const QString &currentChannelId,
                           std::weak_ptr<Channel> outputChannel,
                           QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;

private:
    void filterChannels(const QString &query);
    void setAllChecked(bool checked);
    void handleChannelClicked(QTreeWidgetItem *item);
    void updateActionState();

    void beginStatusCheck();
    void processNextStatusBatch();
    void applyStatusResult(const QVector<QString> &channelIds,
                           const QHash<QString, bool> &statuses,
                           const QString &error = {});
    void finishStatusCheck();

    void beginAction();
    bool checkAuthentication();
    void processNextChannel();
    void continueAfterAction();
    void finishAction(bool stopped = false);
    void requestStop();
    void publishResult(const QString &text) const;

    [[nodiscard]] QString actionVerb(bool capitalized = false) const;
    [[nodiscard]] QString pastActionVerb(bool capitalized = false) const;

    CrossChannelAction action_;
    QString targetId_;
    QString targetLogin_;
    QString targetDisplayName_;
    QString actingLogin_;
    QString oauthToken_;
    std::weak_ptr<Channel> outputChannel_;
    QHash<QString, QTreeWidgetItem *> channelItems_;
    QVector<QTreeWidgetItem *> pendingItems_;
    QVector<QString> statusChannelIds_;
    int statusChecked_ = 0;
    int statusCheckFailures_ = 0;
    int selectionAnchorRow_ = -1;
    QTreeWidgetItem *pressedItem_{};
    int pressedCheckState_ = Qt::Unchecked;
    int actionTotal_ = 0;
    int completed_ = 0;
    int failed_ = 0;
    QString authenticationError_;
    bool statusCheckInFlight_ = false;
    bool actionInFlight_ = false;
    bool stopRequested_ = false;
    pajlada::Signals::SignalHolder authConnections_;

    QLineEdit *search_{};
    QTreeWidget *channels_{};
    QLineEdit *reason_{};
    QLabel *selectionStatus_{};
    QProgressBar *progress_{};
    QPushButton *selectAll_{};
    QPushButton *clear_{};
    QPushButton *cancel_{};
    QPushButton *actionButton_{};
};

}  // namespace chatterino

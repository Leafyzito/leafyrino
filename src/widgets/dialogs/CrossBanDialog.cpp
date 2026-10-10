#include "widgets/dialogs/CrossBanDialog.hpp"

#include "common/Channel.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "singletons/Settings.hpp"
#include "widgets/dialogs/MoltorinoDialogTheme.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QFont>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace chatterino {

namespace {

constexpr int STATUS_BATCH_SIZE = 25;

enum ChannelItemRole {
    ChannelIdRole = Qt::UserRole,
    ChannelLoginRole,
    CurrentChannelRole,
    OperationFailedRole,
};

QString channelLabel(const MoltorinoAuthChannel &channel)
{
    if (!channel.displayName.trimmed().isEmpty())
    {
        return channel.displayName.trimmed();
    }
    if (!channel.login.trimmed().isEmpty())
    {
        return channel.login.trimmed();
    }
    return QStringLiteral("Channel %1").arg(channel.id.right(6));
}

QString channelCount(int count)
{
    return count == 1 ? QStringLiteral("1 channel")
                      : QStringLiteral("%1 channels").arg(count);
}

}  // namespace

CrossBanDialog::CrossBanDialog(CrossChannelAction action, QString targetId,
                               QString targetLogin, QString targetDisplayName,
                               QString actingLogin, QString oauthToken,
                               QVector<MoltorinoAuthChannel> channels,
                               QString currentChannelId,
                               std::weak_ptr<Channel> outputChannel,
                               QWidget *parent)
    : QDialog(parent)
    , action_(action)
    , targetId_(std::move(targetId))
    , targetLogin_(std::move(targetLogin))
    , targetDisplayName_(std::move(targetDisplayName))
    , actingLogin_(std::move(actingLogin))
    , oauthToken_(std::move(oauthToken))
    , outputChannel_(std::move(outputChannel))
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    getSettings()->moltorinoAuthAccounts.connect(
        [this](const QString &, auto) {
            this->checkAuthentication();
            this->updateActionState();
        },
        this->authConnections_, false);
    this->setWindowTitle(QStringLiteral("Cross %1 %2")
                             .arg(this->actionVerb(true), targetLogin_));
    this->setMinimumSize(420, 330);
    this->resize(460, action_ == CrossChannelAction::Ban ? 380 : 350);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    const auto displayName =
        targetDisplayName_.isEmpty() ? targetLogin_ : targetDisplayName_;
    auto *account =
        new QLabel(QStringLiteral("%1 <b>%2</b> as %3")
                       .arg(this->actionVerb(true), displayName.toHtmlEscaped(),
                            actingLogin_.toHtmlEscaped()),
                   this);
    root->addWidget(account);

    auto *tools = new QHBoxLayout;
    tools->setSpacing(6);
    this->search_ = new QLineEdit(this);
    this->search_->setPlaceholderText("Find a channel");
    this->search_->setClearButtonEnabled(true);
    this->selectAll_ = new QPushButton("Select all", this);
    this->clear_ = new QPushButton("Clear", this);
    tools->addWidget(this->search_, 1);
    tools->addWidget(this->selectAll_);
    tools->addWidget(this->clear_);
    root->addLayout(tools);

    this->channels_ = new QTreeWidget(this);
    this->channels_->setColumnCount(2);
    this->channels_->setHeaderLabels({"Channel", "Status"});
    this->channels_->setRootIsDecorated(false);
    this->channels_->setAlternatingRowColors(true);
    this->channels_->setUniformRowHeights(true);
    this->channels_->setSelectionMode(QAbstractItemView::NoSelection);
    this->channels_->header()->setStretchLastSection(false);
    this->channels_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    this->channels_->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    this->channels_->setColumnWidth(1, 105);
    root->addWidget(this->channels_, 1);

    QHash<QString, MoltorinoAuthChannel> unique;
    for (auto &channel : channels)
    {
        channel.id = channel.id.trimmed();
        channel.login = channel.login.trimmed().toLower();
        if (channel.id.isEmpty() || channel.login.isEmpty())
        {
            continue;
        }
        unique.insert(channel.id, std::move(channel));
    }
    channels = unique.values();
    std::ranges::sort(channels, [](const auto &a, const auto &b) {
        return channelLabel(a).compare(channelLabel(b), Qt::CaseInsensitive) <
               0;
    });

    for (const auto &channel : channels)
    {
        auto *item = new QTreeWidgetItem(this->channels_);
        item->setText(0, channelLabel(channel));
        item->setText(1, "Checking...");
        item->setData(0, ChannelIdRole, channel.id);
        item->setData(0, ChannelLoginRole, channel.login);
        item->setData(0, CurrentChannelRole, channel.id == currentChannelId);
        item->setData(0, OperationFailedRole, false);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, Qt::Unchecked);
        item->setDisabled(true);

        const bool isCurrent = channel.id == currentChannelId;
        item->setToolTip(
            0, isCurrent
                   ? QStringLiteral("#%1\nCurrent channel").arg(channel.login)
                   : QStringLiteral("#%1").arg(channel.login));
        if (isCurrent)
        {
            auto font = item->font(0);
            font.setBold(true);
            item->setFont(0, font);
        }
        this->channelItems_.insert(channel.id, item);
        this->statusChannelIds_.push_back(channel.id);
    }

    if (action_ == CrossChannelAction::Ban)
    {
        auto *reasonRow = new QHBoxLayout;
        reasonRow->setSpacing(7);
        reasonRow->addWidget(new QLabel("Reason:", this));
        this->reason_ = new QLineEdit(this);
        this->reason_->setPlaceholderText(
            "Optional reason shown to the user and other mods");
        this->reason_->setMaxLength(500);
        reasonRow->addWidget(this->reason_, 1);
        root->addLayout(reasonRow);
    }

    auto *footer = new QHBoxLayout;
    footer->setSpacing(6);
    this->selectionStatus_ = new QLabel("0 selected", this);
    this->selectionStatus_->setMinimumWidth(76);
    footer->addWidget(this->selectionStatus_, 1);

    this->progress_ = new QProgressBar(this);
    this->progress_->setFixedWidth(132);
    this->progress_->setTextVisible(true);
    footer->addWidget(this->progress_);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    this->cancel_ = buttons->button(QDialogButtonBox::Cancel);
    this->cancel_->setMinimumWidth(70);
    this->actionButton_ = buttons->addButton(
        action_ == CrossChannelAction::Ban ? "Ban selected" : "Unban selected",
        action_ == CrossChannelAction::Ban ? QDialogButtonBox::DestructiveRole
                                           : QDialogButtonBox::AcceptRole);
    this->actionButton_->setMinimumWidth(122);
    footer->addWidget(buttons);
    root->addLayout(footer);

    QObject::connect(this->search_, &QLineEdit::textChanged, this,
                     &CrossBanDialog::filterChannels);
    QObject::connect(this->selectAll_, &QPushButton::clicked, this, [this] {
        this->setAllChecked(true);
    });
    QObject::connect(this->clear_, &QPushButton::clicked, this, [this] {
        this->setAllChecked(false);
    });
    QObject::connect(this->channels_, &QTreeWidget::itemChanged, this, [this] {
        this->updateActionState();
    });
    QObject::connect(this->channels_, &QTreeWidget::itemPressed, this,
                     [this](QTreeWidgetItem *item, int) {
                         this->pressedItem_ = item;
                         this->pressedCheckState_ = item == nullptr
                                                        ? Qt::Unchecked
                                                        : item->checkState(0);
                     });
    QObject::connect(this->channels_, &QTreeWidget::itemClicked, this,
                     [this](QTreeWidgetItem *item, int) {
                         this->handleChannelClicked(item);
                     });
    QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                     &CrossBanDialog::reject);
    QObject::connect(this->actionButton_, &QPushButton::clicked, this,
                     &CrossBanDialog::beginAction);

    installMoltorinoDialogTheme(this);
    this->updateActionState();
    QTimer::singleShot(0, this, &CrossBanDialog::beginStatusCheck);
}

void CrossBanDialog::showDialog(
    CrossChannelAction action, const QString &targetId,
    const QString &targetLogin, const QString &targetDisplayName,
    const QString &actingLogin, const QString &oauthToken,
    const QVector<MoltorinoAuthChannel> &channels,
    const QString &currentChannelId, std::weak_ptr<Channel> outputChannel,
    QWidget *parent)
{
    auto *dialog =
        new CrossBanDialog(action, targetId, targetLogin, targetDisplayName,
                           actingLogin, oauthToken, channels, currentChannelId,
                           std::move(outputChannel), parent);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void CrossBanDialog::filterChannels(const QString &query)
{
    const auto needle = query.trimmed();
    for (int row = 0; row < this->channels_->topLevelItemCount(); ++row)
    {
        auto *item = this->channels_->topLevelItem(row);
        const auto login = item->data(0, ChannelLoginRole).toString();
        item->setHidden(!needle.isEmpty() &&
                        !item->text(0).contains(needle, Qt::CaseInsensitive) &&
                        !login.contains(needle, Qt::CaseInsensitive));
    }
}

void CrossBanDialog::setAllChecked(bool checked)
{
    const QSignalBlocker blocker(this->channels_);
    for (int row = 0; row < this->channels_->topLevelItemCount(); ++row)
    {
        auto *item = this->channels_->topLevelItem(row);
        if (item->flags().testFlag(Qt::ItemIsEnabled) &&
            (!checked || !item->isHidden()))
        {
            item->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
        }
    }
    this->selectionAnchorRow_ = -1;
    this->updateActionState();
}

void CrossBanDialog::handleChannelClicked(QTreeWidgetItem *item)
{
    if (item == nullptr || this->statusCheckInFlight_ || this->actionInFlight_)
    {
        return;
    }

    const auto row = this->channels_->indexOfTopLevelItem(item);
    if (row < 0)
    {
        return;
    }

    if (this->pressedItem_ == item &&
        item->checkState(0) == this->pressedCheckState_)
    {
        item->setCheckState(0, item->checkState(0) == Qt::Checked
                                   ? Qt::Unchecked
                                   : Qt::Checked);
    }
    this->pressedItem_ = nullptr;

    const bool extend =
        QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
    if (!extend || this->selectionAnchorRow_ < 0)
    {
        this->selectionAnchorRow_ = row;
        return;
    }

    const auto state = item->checkState(0);
    const auto first = std::min(row, this->selectionAnchorRow_);
    const auto last = std::max(row, this->selectionAnchorRow_);
    const QSignalBlocker blocker(this->channels_);
    for (int index = first; index <= last; ++index)
    {
        auto *rangeItem = this->channels_->topLevelItem(index);
        if (!rangeItem->isHidden() &&
            rangeItem->flags().testFlag(Qt::ItemIsEnabled))
        {
            rangeItem->setCheckState(0, state);
        }
    }
    this->updateActionState();
}

void CrossBanDialog::updateActionState()
{
    if (this->actionInFlight_)
    {
        this->selectionStatus_->setText(
            QStringLiteral("%1 queued").arg(this->actionTotal_));
        this->actionButton_->setEnabled(false);
        this->selectAll_->setEnabled(false);
        this->clear_->setEnabled(false);
        this->search_->setEnabled(false);
        if (this->reason_ != nullptr)
        {
            this->reason_->setEnabled(false);
        }
        this->cancel_->setText(this->stopRequested_ ? "Stopping..." : "Stop");
        this->cancel_->setEnabled(!this->stopRequested_);
        return;
    }

    int selected = 0;
    int available = 0;
    int selectedFailures = 0;
    for (int row = 0; row < this->channels_->topLevelItemCount(); ++row)
    {
        const auto *item = this->channels_->topLevelItem(row);
        if (!item->flags().testFlag(Qt::ItemIsEnabled))
        {
            continue;
        }
        available++;
        if (item->checkState(0) == Qt::Checked)
        {
            selected++;
            selectedFailures +=
                item->data(0, OperationFailedRole).toBool() ? 1 : 0;
        }
    }

    this->selectionStatus_->setText(
        QStringLiteral("%1 selected").arg(selected));

    if (selected > 0 && selectedFailures == selected)
    {
        this->actionButton_->setText(
            QStringLiteral("Retry %1").arg(channelCount(selected)));
    }
    else if (selected > 0)
    {
        this->actionButton_->setText(QStringLiteral("%1 %2").arg(
            this->actionVerb(true), channelCount(selected)));
    }
    else
    {
        this->actionButton_->setText(this->action_ == CrossChannelAction::Ban
                                         ? "Ban selected"
                                         : "Unban selected");
    }

    const bool ready =
        !this->statusCheckInFlight_ && this->authenticationError_.isEmpty();
    this->actionButton_->setEnabled(ready && selected > 0);
    this->selectAll_->setEnabled(ready && available > 0);
    this->clear_->setEnabled(ready && selected > 0);
    this->search_->setEnabled(true);
    if (this->reason_ != nullptr)
    {
        this->reason_->setEnabled(true);
    }

    this->cancel_->setText("Cancel");
    this->cancel_->setEnabled(true);
}

void CrossBanDialog::beginStatusCheck()
{
    this->statusCheckInFlight_ = true;
    this->statusChecked_ = 0;
    this->statusCheckFailures_ = 0;
    this->progress_->setRange(
        0, std::max(1, static_cast<int>(this->statusChannelIds_.size())));
    this->progress_->setValue(0);
    this->progress_->setFormat("Checking %m channels");
    this->progress_->setToolTip(
        "Checking whether this user is banned in each channel");
    this->updateActionState();
    this->processNextStatusBatch();
}

void CrossBanDialog::processNextStatusBatch()
{
    if (this->statusChannelIds_.isEmpty())
    {
        this->finishStatusCheck();
        return;
    }
    if (!this->checkAuthentication())
    {
        const auto remaining = this->statusChannelIds_;
        this->statusChannelIds_.clear();
        this->applyStatusResult(remaining, {}, this->authenticationError_);
        this->finishStatusCheck();
        return;
    }

    const auto batchSize = std::min(
        STATUS_BATCH_SIZE, static_cast<int>(this->statusChannelIds_.size()));
    const auto batch = this->statusChannelIds_.mid(0, batchSize);
    this->statusChannelIds_.remove(0, batchSize);

    const QPointer<CrossBanDialog> self(this);
    TwitchGql::getChatRoomBanStatuses(
        this->targetId_, batch, this->oauthToken_,
        [self, batch](QHash<QString, bool> statuses) {
            if (!self)
            {
                return;
            }
            self->applyStatusResult(batch, statuses);
            QTimer::singleShot(0, self, [self] {
                if (self)
                {
                    self->processNextStatusBatch();
                }
            });
        },
        [self, batch](const QString &error) {
            if (!self)
            {
                return;
            }
            const auto normalized = MoltorinoAuth::normalizeAuthError(
                "checking cross channel ban status", error);
            if (normalized != error)
            {
                self->authenticationError_ = normalized;
                auto remaining = self->statusChannelIds_;
                self->statusChannelIds_.clear();
                self->applyStatusResult(batch, {}, normalized);
                self->applyStatusResult(remaining, {}, normalized);
                self->finishStatusCheck();
                return;
            }
            self->applyStatusResult(batch, {}, normalized);
            QTimer::singleShot(0, self, [self] {
                if (self)
                {
                    self->processNextStatusBatch();
                }
            });
        });
}

void CrossBanDialog::applyStatusResult(const QVector<QString> &channelIds,
                                       const QHash<QString, bool> &statuses,
                                       const QString &error)
{
    const QSignalBlocker blocker(this->channels_);
    for (const auto &channelId : channelIds)
    {
        auto *item = this->channelItems_.value(channelId);
        if (item == nullptr)
        {
            continue;
        }

        const bool known = error.isEmpty() && statuses.contains(channelId);
        const bool banned = known && statuses.value(channelId);
        const bool eligible =
            this->authenticationError_.isEmpty() &&
            (!known ||
             (this->action_ == CrossChannelAction::Ban ? !banned : banned));

        item->setDisabled(!eligible);
        item->setData(0, OperationFailedRole, false);
        item->setCheckState(
            0, eligible && item->data(0, CurrentChannelRole).toBool()
                   ? Qt::Checked
                   : Qt::Unchecked);

        if (!known)
        {
            item->setText(1, this->authenticationError_.isEmpty()
                                 ? "Unknown"
                                 : "Login required");
            const auto detail =
                error.isEmpty()
                    ? QStringLiteral("Twitch did not return a ban status")
                    : error;
            item->setToolTip(
                1,
                QStringLiteral("Could not check ban status: %1").arg(detail));
            this->statusCheckFailures_++;
        }
        else
        {
            item->setText(1, banned ? (eligible ? "Banned" : "Already banned")
                                    : "Not banned");
            item->setToolTip(1, {});
        }
    }

    this->statusChecked_ += channelIds.size();
    this->progress_->setValue(this->statusChecked_);
    this->updateActionState();
}

void CrossBanDialog::finishStatusCheck()
{
    this->statusCheckInFlight_ = false;
    this->progress_->setRange(0, 1);
    this->progress_->setValue(0);
    this->progress_->setFormat("Ready");
    if (!this->authenticationError_.isEmpty())
    {
        this->progress_->setFormat("Login required");
        this->progress_->setToolTip(this->authenticationError_);
    }
    else if (this->statusCheckFailures_ > 0)
    {
        this->progress_->setToolTip(
            QStringLiteral("%1 channel status%2 could not be checked. You can "
                           "still try the action in those channels.")
                .arg(this->statusCheckFailures_)
                .arg(this->statusCheckFailures_ == 1 ? QString() : "es"));
    }
    else
    {
        this->progress_->setToolTip("Ban status is up to date");
    }
    this->updateActionState();
}

void CrossBanDialog::closeEvent(QCloseEvent *event)
{
    if (this->actionInFlight_)
    {
        this->requestStop();
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void CrossBanDialog::reject()
{
    if (this->actionInFlight_)
    {
        this->requestStop();
        return;
    }
    QDialog::reject();
}

bool CrossBanDialog::checkAuthentication()
{
    const auto accounts = MoltorinoAuth::accounts();
    const auto available = std::any_of(
        accounts.begin(), accounts.end(), [this](const auto &account) {
            return account.valid && account.token == this->oauthToken_ &&
                   account.login.compare(this->actingLogin_,
                                         Qt::CaseInsensitive) == 0;
        });
    if (!available)
    {
        this->authenticationError_ =
            "The account used for this action has changed. Reopen the dialog "
            "to choose an available account.";
    }
    return available && this->authenticationError_.isEmpty();
}

void CrossBanDialog::beginAction()
{
    if (this->actionInFlight_ || this->statusCheckInFlight_ ||
        !this->checkAuthentication())
    {
        this->updateActionState();
        return;
    }
    this->pendingItems_.clear();
    for (int row = 0; row < this->channels_->topLevelItemCount(); ++row)
    {
        auto *item = this->channels_->topLevelItem(row);
        if (item->flags().testFlag(Qt::ItemIsEnabled) &&
            item->checkState(0) == Qt::Checked)
        {
            this->pendingItems_.push_back(item);
        }
    }
    if (this->pendingItems_.isEmpty())
    {
        return;
    }

    const auto countText = channelCount(this->pendingItems_.size());
    const QPointer<CrossBanDialog> self(this);
    QPointer<QMessageBox> confirmation = new QMessageBox(
        QMessageBox::Warning,
        QStringLiteral("Confirm cross %1").arg(this->actionVerb()),
        QStringLiteral("%1 %2 in %3?")
            .arg(this->actionVerb(true), targetLogin_, countText),
        QMessageBox::Cancel, this);
    confirmation->setTextFormat(Qt::PlainText);
    auto *confirm = confirmation->addButton(
        QStringLiteral("%1 %2").arg(this->actionVerb(true), countText),
        this->action_ == CrossChannelAction::Ban ? QMessageBox::DestructiveRole
                                                 : QMessageBox::AcceptRole);

    if (this->reason_ != nullptr)
    {
        const auto reason = this->reason_->text().trimmed();
        confirmation->setInformativeText(
            reason.isEmpty()
                ? QStringLiteral(
                      "No reason is set. Other moderators will not see why "
                      "this ban was made.")
                : QStringLiteral("Reason: %1").arg(reason));
        if (reason.isEmpty())
        {
            confirm->setText(
                QStringLiteral("Ban %1 without reason").arg(countText));
        }
    }
    confirmation->setDefaultButton(QMessageBox::Cancel);
    confirmation->setEscapeButton(QMessageBox::Cancel);
    installMoltorinoDialogTheme(confirmation);
    confirmation->exec();
    if (!self || !confirmation)
    {
        return;
    }
    const bool confirmed = confirmation->clickedButton() == confirm;
    delete confirmation;
    if (!confirmed || !this->checkAuthentication())
    {
        return;
    }

    this->actionInFlight_ = true;
    this->stopRequested_ = false;
    this->actionTotal_ = this->pendingItems_.size();
    this->completed_ = 0;
    this->failed_ = 0;
    this->selectionAnchorRow_ = -1;
    this->channels_->setEnabled(false);
    this->progress_->setRange(0, this->actionTotal_);
    this->progress_->setValue(0);
    this->progress_->setFormat(this->action_ == CrossChannelAction::Ban
                                   ? "Banning %m channels"
                                   : "Unbanning %m channels");
    this->progress_->setToolTip("Select Stop to finish the current request and "
                                "leave the rest selected");
    this->updateActionState();
    this->processNextChannel();
}

void CrossBanDialog::processNextChannel()
{
    if (this->pendingItems_.isEmpty())
    {
        this->finishAction();
        return;
    }
    if (this->stopRequested_ || !this->checkAuthentication())
    {
        this->finishAction(true);
        return;
    }

    auto *item = this->pendingItems_.takeFirst();
    const auto channelId = item->data(0, ChannelIdRole).toString();
    item->setText(1, this->action_ == CrossChannelAction::Ban ? "Banning..."
                                                              : "Unbanning...");

    const QPointer<CrossBanDialog> self(this);
    const auto success = [self, item] {
        if (!self)
        {
            return;
        }
        self->completed_++;
        item->setText(1, self->action_ == CrossChannelAction::Ban
                             ? "Banned"
                             : "Not banned");
        item->setToolTip(1, {});
        item->setData(0, OperationFailedRole, false);
        item->setCheckState(0, Qt::Unchecked);
        item->setDisabled(true);
        self->continueAfterAction();
    };
    const auto failure = [self, item](const QString &error) {
        if (!self)
        {
            return;
        }
        const auto normalized = MoltorinoAuth::normalizeAuthError(
            QStringLiteral("running a cross %1").arg(self->actionVerb()),
            error);
        self->failed_++;
        item->setText(1, "Failed");
        item->setToolTip(1, normalized);
        item->setData(0, OperationFailedRole, true);
        if (normalized != error)
        {
            self->authenticationError_ = normalized;
            self->stopRequested_ = true;
        }
        self->continueAfterAction();
    };

    if (this->action_ == CrossChannelAction::Ban)
    {
        TwitchGql::banUserFromChatRoom(channelId, this->targetLogin_,
                                       this->reason_->text().trimmed(),
                                       this->oauthToken_, success, failure);
    }
    else
    {
        TwitchGql::unbanUserFromChatRoom(channelId, this->targetLogin_,
                                         this->oauthToken_, success, failure);
    }
}

void CrossBanDialog::continueAfterAction()
{
    this->progress_->setValue(this->completed_ + this->failed_);
    if (this->pendingItems_.isEmpty())
    {
        this->finishAction();
        return;
    }
    if (this->stopRequested_)
    {
        this->finishAction(true);
        return;
    }

    const QPointer<CrossBanDialog> self(this);
    QTimer::singleShot(200, self, [self] {
        if (self)
        {
            self->processNextChannel();
        }
    });
}

void CrossBanDialog::finishAction(bool stopped)
{
    const auto notAttempted = this->pendingItems_.size();
    if (stopped || !this->authenticationError_.isEmpty())
    {
        for (auto *item : this->pendingItems_)
        {
            item->setText(1, "Not tried");
            item->setToolTip(1, {});
            item->setData(0, OperationFailedRole, false);
        }
    }

    this->actionInFlight_ = false;
    this->stopRequested_ = false;
    this->channels_->setEnabled(true);
    this->progress_->setRange(0, std::max(1, this->actionTotal_));
    this->progress_->setValue(this->completed_ + this->failed_);
    if (!this->authenticationError_.isEmpty())
    {
        this->progress_->setFormat("Login required");
    }
    else if (stopped)
    {
        this->progress_->setFormat(QStringLiteral("Stopped: %1 / %2")
                                       .arg(this->completed_ + this->failed_)
                                       .arg(this->actionTotal_));
    }
    else if (this->failed_ == 0)
    {
        this->progress_->setFormat(QStringLiteral("%1 %2")
                                       .arg(this->completed_)
                                       .arg(this->pastActionVerb()));
    }
    else
    {
        this->progress_->setFormat(QStringLiteral("%1 done, %2 failed")
                                       .arg(this->completed_)
                                       .arg(this->failed_));
    }
    this->progress_->setToolTip(
        !this->authenticationError_.isEmpty() ? this->authenticationError_
        : this->failed_ > 0
            ? QStringLiteral("Hover a failed channel status for details")
            : QString());
    this->pendingItems_.clear();
    this->updateActionState();

    const auto featureName = this->action_ == CrossChannelAction::Ban
                                 ? QStringLiteral("Cross ban")
                                 : QStringLiteral("Cross unban");
    QString result;
    if (!this->authenticationError_.isEmpty())
    {
        result =
            QStringLiteral("%1 for %2 stopped: %3 succeeded, %4 failed, %5 not "
                           "tried. %6")
                .arg(featureName, targetLogin_)
                .arg(this->completed_)
                .arg(this->failed_)
                .arg(notAttempted)
                .arg(this->authenticationError_);
    }
    else if (stopped)
    {
        result =
            QStringLiteral("%1 for %2 stopped: %3 succeeded, %4 failed, %5 not "
                           "tried.")
                .arg(featureName, targetLogin_)
                .arg(this->completed_)
                .arg(this->failed_)
                .arg(notAttempted);
    }
    else if (this->failed_ == 0)
    {
        result = QStringLiteral("%1 %2 in %3.")
                     .arg(this->pastActionVerb(true), targetLogin_,
                          channelCount(this->completed_));
    }
    else
    {
        result = QStringLiteral("%1 for %2: %3 succeeded, %4 failed.")
                     .arg(featureName, targetLogin_)
                     .arg(this->completed_)
                     .arg(this->failed_);
    }
    this->publishResult(result);
}

void CrossBanDialog::requestStop()
{
    if (!this->actionInFlight_ || this->stopRequested_)
    {
        return;
    }
    this->stopRequested_ = true;
    this->progress_->setFormat("Stopping...");
    this->progress_->setToolTip("The current request will finish. Remaining "
                                "channels will stay selected.");
    this->updateActionState();
}

void CrossBanDialog::publishResult(const QString &text) const
{
    if (const auto channel = this->outputChannel_.lock())
    {
        channel->addSystemMessage(text);
    }
}

QString CrossBanDialog::actionVerb(bool capitalized) const
{
    if (this->action_ == CrossChannelAction::Ban)
    {
        return capitalized ? QStringLiteral("Ban") : QStringLiteral("ban");
    }
    return capitalized ? QStringLiteral("Unban") : QStringLiteral("unban");
}

QString CrossBanDialog::pastActionVerb(bool capitalized) const
{
    if (this->action_ == CrossChannelAction::Ban)
    {
        return capitalized ? QStringLiteral("Banned")
                           : QStringLiteral("banned");
    }
    return capitalized ? QStringLiteral("Unbanned")
                       : QStringLiteral("unbanned");
}

}  // namespace chatterino

#include "controllers/automod/AutoModReviewController.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "debug/AssertInGuiThread.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/api/HelixEnums.hpp"
#include "providers/twitch/eventsub/MessageBuilder.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Settings.hpp"
#include "util/FormatTime.hpp"
#include "util/QStringHash.hpp"

#include <QStringBuilder>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace chatterino::automod {

namespace {

constexpr qsizetype MAX_REVIEW_ITEMS = 300;
constexpr qsizetype MAX_NOTIFICATION_IDS = 512;
constexpr qsizetype CONTEXT_MESSAGE_LIMIT = 3;
constexpr size_t CONTEXT_SCAN_LIMIT = 500;

QString autoModMessageID(const ReviewItem &item)
{
    return u"automod_" % item.messageID;
}

QString autoModErrorText(HelixAutoModMessageError error)
{
    switch (error)
    {
        case HelixAutoModMessageError::MessageAlreadyProcessed:
            return u"Another moderator already handled this message."_s;
        case HelixAutoModMessageError::UserNotAuthenticated:
            return u"Add your Twitch account again to review messages."_s;
        case HelixAutoModMessageError::UserNotAuthorized:
            return u"You no longer have permission to review this message."_s;
        case HelixAutoModMessageError::MessageNotFound:
            return u"Twitch no longer has this held message."_s;
        case HelixAutoModMessageError::Unknown:
        default:
            return u"Twitch could not complete the action."_s;
    }
}

QString punishmentErrorText(HelixBanUserError error, const QString &message)
{
    switch (error)
    {
        case HelixBanUserError::ConflictingOperation:
            return u"Twitch reported a conflicting moderation action."_s;
        case HelixBanUserError::Forwarded:
            return message.isEmpty() ? u"Twitch rejected the action."_s
                                     : message;
        case HelixBanUserError::Ratelimited:
            return u"Twitch rate limited the moderation action."_s;
        case HelixBanUserError::TargetBanned:
            return u"The user is already banned."_s;
        case HelixBanUserError::CannotBanUser:
            return u"This user cannot be moderated by the current account."_s;
        case HelixBanUserError::UserMissingScope:
            return u"Your Twitch account is missing moderation permissions. "
                   "Add it again and try again."_s;
        case HelixBanUserError::UserNotAuthorized:
            return u"You no longer have permission to moderate this channel."_s;
        case HelixBanUserError::Unknown:
        default:
            return u"Twitch could not complete the moderation action."_s;
    }
}

bool isResolvedState(ReviewState state)
{
    return state == ReviewState::Approved || state == ReviewState::Denied ||
           state == ReviewState::HandledElsewhere ||
           state == ReviewState::Expired;
}

}  // namespace

ReviewGroup reviewGroup(const ReviewItem &item)
{
    if (item.secondaryInProgress)
    {
        return ReviewGroup::Open;
    }
    if (item.state == ReviewState::Failed ||
        item.state == ReviewState::PermissionLost || item.secondaryFailed)
    {
        return ReviewGroup::Failed;
    }
    if (isResolvedState(item.state))
    {
        return ReviewGroup::Resolved;
    }
    return ReviewGroup::Open;
}

bool isActionable(const ReviewItem &item)
{
    return item.actionsAllowed &&
           (item.state == ReviewState::Open ||
            item.state == ReviewState::Failed ||
            item.state == ReviewState::PermissionLost || item.secondaryFailed);
}

QString reviewStateLabel(const ReviewItem &item)
{
    if (item.secondaryInProgress)
    {
        return item.secondaryAction == ReviewAction::Ban ? u"Banning…"_s
                                                         : u"Timing out…"_s;
    }
    if (item.secondaryFailed)
    {
        return u"Partial failure"_s;
    }

    switch (item.state)
    {
        case ReviewState::Open:
            return item.actionsAllowed ? u"Open"_s : u"Read only"_s;
        case ReviewState::Approving:
            return u"Approving…"_s;
        case ReviewState::Denying:
            return u"Denying…"_s;
        case ReviewState::Approved:
            return u"Approved"_s;
        case ReviewState::Denied:
            return u"Denied"_s;
        case ReviewState::HandledElsewhere:
            return u"Handled elsewhere"_s;
        case ReviewState::Expired:
            return u"Expired"_s;
        case ReviewState::Failed:
            return u"Failed"_s;
        case ReviewState::PermissionLost:
            return u"Permission lost"_s;
    }
    return u"Unknown"_s;
}

QString reviewActionLabel(ReviewAction action)
{
    switch (action)
    {
        case ReviewAction::Approve:
            return u"approve"_s;
        case ReviewAction::Deny:
            return u"deny"_s;
        case ReviewAction::DenyAndTimeout:
            return u"deny and timeout"_s;
        case ReviewAction::DenyAndBan:
            return u"deny and ban"_s;
        case ReviewAction::Timeout:
            return u"timeout"_s;
        case ReviewAction::Ban:
            return u"ban"_s;
        case ReviewAction::None:
            return u"action"_s;
    }
    return u"action"_s;
}

AutoModReviewController::AutoModReviewController() = default;
AutoModReviewController::~AutoModReviewController() = default;

void AutoModReviewController::initialize()
{
    // currentUserChanged is a boost signal
    this->accountConnection_ =
        getApp()->getAccounts()->twitch.currentUserChanged.connect([this] {
            ++this->accountGeneration_;
            this->refreshPermissions();
        });
    const auto republish = [this](const auto &) {
        for (auto it = this->entries_.begin(); it != this->entries_.end(); ++it)
        {
            this->publish(it.value(), false);
        }
    };
    getSettings()->advancedAutoModInChat.connect(republish,
                                                 this->signalHolder_);
    getSettings()->autoModReviewShowReason.connect(republish,
                                                   this->signalHolder_);
    getSettings()->autoModReviewShowContext.connect(republish,
                                                    this->signalHolder_);
    getSettings()->autoModReviewContextOrder.connect(republish,
                                                     this->signalHolder_);
    getSettings()->autoModReviewShowShortcutHints.connect(republish,
                                                          this->signalHolder_);
}

QString AutoModReviewController::makeKey(QStringView broadcasterID,
                                         QStringView messageID)
{
    return broadcasterID % u':' % messageID;
}

bool AutoModReviewController::rememberNotification(
    const QString &notificationID)
{
    if (notificationID.isEmpty())
    {
        return true;
    }
    if (this->notificationIDs_.contains(notificationID))
    {
        return false;
    }
    this->notificationIDs_.insert(notificationID);
    this->notificationOrder_.enqueue(notificationID);
    while (this->notificationOrder_.size() > MAX_NOTIFICATION_IDS)
    {
        this->notificationIDs_.remove(this->notificationOrder_.dequeue());
    }
    return true;
}

void AutoModReviewController::ingestHold(
    HoldData data, const std::shared_ptr<TwitchChannel> &sourceChannel)
{
    assertInGuiThread();
    const auto notificationKey = data.notificationID.isEmpty()
                                     ? QString{}
                                     : u"hold:"_s % data.notificationID;
    if (!this->rememberNotification(notificationKey) ||
        data.broadcasterID.isEmpty() || data.messageID.isEmpty())
    {
        return;
    }
    this->watchChannelPermissions(data.broadcasterID, sourceChannel);

    const auto key = makeKey(data.broadcasterID, data.messageID);
    if (auto existing = this->entries_.find(key);
        existing != this->entries_.end())
    {
        const auto wasAllowed = existing->item.actionsAllowed;
        existing->sourceChannel = sourceChannel;
        this->updateActionsAllowed(existing.value());
        if (wasAllowed != existing->item.actionsAllowed)
        {
            this->publish(existing.value(), false);
            this->itemChanged.invoke(key);
        }
        return;
    }

    Entry entry;
    auto &item = entry.item;
    item.key = key;
    item.broadcasterID = std::move(data.broadcasterID);
    item.broadcasterLogin = std::move(data.broadcasterLogin);
    item.broadcasterName = std::move(data.broadcasterName);
    item.userID = std::move(data.userID);
    item.userLogin = std::move(data.userLogin);
    item.userName = std::move(data.userName);
    item.messageID = std::move(data.messageID);
    item.messageText = std::move(data.messageText);
    item.receivedAt = data.receivedAt;
    item.updatedAt = data.receivedAt;
    item.blockedTerm = data.blockedTerm;
    item.reasonSummary = std::move(data.reasonSummary);
    item.reasonCategory = std::move(data.reasonCategory);
    item.reasonLevel = data.reasonLevel;
    item.matchedFragments = std::move(data.matchedFragments);
    item.termOwnerLogin = std::move(data.termOwnerLogin);
    if (!data.twitchMatches.empty())
    {
        item.highlightMatches =
            std::make_shared<const std::vector<HighlightMatch>>(
                std::move(data.twitchMatches));
    }
    entry.sourceChannel = sourceChannel;
    this->updateActionsAllowed(entry);

    this->order_.push_back(key);
    auto inserted = this->entries_.insert(key, std::move(entry));
    // only a new hold plays the sound and flashes the taskbar, later
    // rebuilds of the same review stay quiet
    auto [message, alert] = this->publish(inserted.value(), true);
    if (const auto source = inserted->sourceChannel.lock())
    {
        MessageBuilder::triggerHighlights(source.get(), message, alert);
    }
    this->prune();
    this->itemAdded.invoke(key);
    this->countsChanged.invoke();
}

void AutoModReviewController::ingestUpdate(
    UpdateData data, const std::shared_ptr<TwitchChannel> &sourceChannel)
{
    assertInGuiThread();
    const auto notificationKey = data.notificationID.isEmpty()
                                     ? QString{}
                                     : u"update:"_s % data.notificationID;
    if (!this->rememberNotification(notificationKey) ||
        data.broadcasterID.isEmpty() || data.messageID.isEmpty())
    {
        return;
    }
    this->watchChannelPermissions(data.broadcasterID, sourceChannel);

    const auto key = makeKey(data.broadcasterID, data.messageID);
    auto it = this->entries_.find(key);
    const bool orphan = it == this->entries_.end();
    if (orphan)
    {
        Entry entry;
        entry.item.key = key;
        entry.item.broadcasterID = data.broadcasterID;
        entry.item.messageID = data.messageID;
        entry.item.receivedAt = data.receivedAt;
        entry.sourceChannel = sourceChannel;
        this->order_.push_back(key);
        it = this->entries_.insert(key, std::move(entry));
    }

    auto &entry = it.value();
    auto &item = entry.item;
    const auto previousPendingAction = item.pendingAction;
    const auto previousActionAccountID = item.actionAccountID;
    const auto previousTimeoutSeconds = item.timeoutSeconds;
    const auto previousModerationReason = item.moderationReason;
    entry.sourceChannel = sourceChannel;
    item.broadcasterID = std::move(data.broadcasterID);
    item.broadcasterLogin = std::move(data.broadcasterLogin);
    item.broadcasterName = std::move(data.broadcasterName);
    item.userID = std::move(data.userID);
    item.userLogin = std::move(data.userLogin);
    item.userName = std::move(data.userName);
    item.messageText = std::move(data.messageText);
    item.updatedAt = data.receivedAt;
    item.blockedTerm = data.blockedTerm;
    item.reasonSummary = std::move(data.reasonSummary);
    item.reasonCategory = std::move(data.reasonCategory);
    item.reasonLevel = data.reasonLevel;
    item.matchedFragments = std::move(data.matchedFragments);
    item.termOwnerLogin = std::move(data.termOwnerLogin);
    if (orphan && !data.twitchMatches.empty())
    {
        item.highlightMatches =
            std::make_shared<const std::vector<HighlightMatch>>(
                std::move(data.twitchMatches));
    }
    item.moderatorID = std::move(data.moderatorID);
    item.moderatorLogin = std::move(data.moderatorLogin);
    item.moderatorName = std::move(data.moderatorName);
    item.pendingAction = ReviewAction::None;
    item.failedAction = ReviewAction::None;

    const auto status = data.status.toLower();
    ReviewAction confirmedSecondary = ReviewAction::None;
    if (status == u"approved")
    {
        item.state = ReviewState::Approved;
        item.statusDetail.clear();
        item.secondaryInProgress = false;
        item.secondaryFailed = false;
        item.secondaryAction = ReviewAction::None;
        item.actionAccountID.clear();
        item.actionGeneration = ++this->actionGeneration_;
    }
    else if (status == u"denied")
    {
        item.state = ReviewState::Denied;
        const bool compoundAlreadyStarted =
            item.secondaryInProgress || item.secondaryFailed ||
            item.secondaryAction == ReviewAction::Timeout ||
            item.secondaryAction == ReviewAction::Ban;
        const bool pendingCompound =
            previousPendingAction == ReviewAction::DenyAndTimeout ||
            previousPendingAction == ReviewAction::DenyAndBan;
        const bool confirmsOurDecision =
            pendingCompound && !previousActionAccountID.isEmpty() &&
            item.moderatorID == previousActionAccountID;

        if (confirmsOurDecision && !compoundAlreadyStarted)
        {
            confirmedSecondary =
                previousPendingAction == ReviewAction::DenyAndBan
                    ? ReviewAction::Ban
                    : ReviewAction::Timeout;
            item.secondaryAction = confirmedSecondary;
            item.actionGeneration = ++this->actionGeneration_;
        }
        else if (!compoundAlreadyStarted)
        {
            item.statusDetail.clear();
            item.secondaryFailed = false;
            item.secondaryAction = ReviewAction::None;
            item.actionAccountID.clear();
            item.actionGeneration = ++this->actionGeneration_;
        }
        else if (!item.secondaryInProgress)
        {
            item.actionGeneration = ++this->actionGeneration_;
        }
    }
    else if (status == u"expired")
    {
        item.state = ReviewState::Expired;
        item.statusDetail.clear();
        item.secondaryInProgress = false;
        item.secondaryFailed = false;
        item.secondaryAction = ReviewAction::None;
        item.actionAccountID.clear();
        item.actionGeneration = ++this->actionGeneration_;
    }
    else
    {
        item.state = ReviewState::HandledElsewhere;
        item.statusDetail =
            u"Twitch resolved this message as “%1”."_s.arg(data.status);
        item.secondaryInProgress = false;
        item.secondaryFailed = false;
        item.secondaryAction = ReviewAction::None;
        item.actionAccountID.clear();
        item.actionGeneration = ++this->actionGeneration_;
    }

    this->updateActionsAllowed(entry);
    this->publish(entry, true);
    this->prune();
    if (orphan)
    {
        this->itemAdded.invoke(key);
    }
    else
    {
        this->itemChanged.invoke(key);
    }
    this->countsChanged.invoke();

    if (confirmedSecondary != ReviewAction::None)
    {
        this->performSecondary(key, confirmedSecondary, previousTimeoutSeconds,
                               previousModerationReason,
                               previousActionAccountID);
    }
}

void AutoModReviewController::approve(const QString &key)
{
    this->performDecision(key, ReviewAction::Approve);
}

void AutoModReviewController::deny(const QString &key)
{
    this->performDecision(key, ReviewAction::Deny);
}

void AutoModReviewController::denyAndTimeout(const QString &key,
                                             int durationSeconds,
                                             QString reason)
{
    this->performDecision(key, ReviewAction::DenyAndTimeout, durationSeconds,
                          std::move(reason));
}

void AutoModReviewController::denyAndBan(const QString &key, QString reason)
{
    this->performDecision(key, ReviewAction::DenyAndBan, 0, std::move(reason));
}

void AutoModReviewController::performDecision(const QString &key,
                                              ReviewAction action,
                                              int timeoutSeconds,
                                              QString reason, bool retrying)
{
    assertInGuiThread();
    auto it = this->entries_.find(key);
    if (it == this->entries_.end())
    {
        return;
    }
    auto &entry = it.value();
    auto &item = entry.item;
    this->updateActionsAllowed(entry);
    const bool validState = retrying
                                ? item.state == ReviewState::Failed ||
                                      item.state == ReviewState::PermissionLost
                                : item.state == ReviewState::Open;
    if (!validState)
    {
        return;
    }
    item.timeoutSeconds = timeoutSeconds;
    item.moderationReason = std::move(reason);
    if (!item.actionsAllowed)
    {
        item.state = ReviewState::PermissionLost;
        item.failedAction = action;
        item.statusDetail =
            u"You no longer have permission to review this message."_s;
        item.updatedAt = QDateTime::currentDateTimeUtc();
        this->publish(entry, false);
        this->itemChanged.invoke(key);
        this->countsChanged.invoke();
        return;
    }

    const auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!account || account->isAnon())
    {
        item.state = ReviewState::PermissionLost;
        item.failedAction = action;
        item.statusDetail = u"Log in with a moderator account to continue."_s;
        this->publish(entry, false);
        this->itemChanged.invoke(key);
        this->countsChanged.invoke();
        return;
    }

    const bool approveAction = action == ReviewAction::Approve;
    item.state = approveAction ? ReviewState::Approving : ReviewState::Denying;
    item.pendingAction = action;
    item.failedAction = ReviewAction::None;
    item.actionAccountID = account->getUserId();
    entry.actionAccountGeneration = this->accountGeneration_;
    item.secondaryFailed = false;
    item.secondaryError.clear();
    item.actionGeneration = ++this->actionGeneration_;
    const auto generation = item.actionGeneration;
    this->publish(entry, false);
    this->itemChanged.invoke(key);
    this->countsChanged.invoke();

    const auto helixAction = approveAction ? u"ALLOW"_s : u"DENY"_s;
    getHelix()->manageAutoModMessages(
        account->getUserId(), item.messageID, helixAction,
        [this, key, generation, action, account] {
            auto successIt = this->entries_.find(key);
            if (successIt == this->entries_.end() ||
                successIt->item.actionGeneration != generation)
            {
                return;
            }
            auto &successEntry = successIt.value();
            auto &successItem = successEntry.item;
            successItem.pendingAction = ReviewAction::None;
            successItem.failedAction = ReviewAction::None;
            successItem.moderatorID = account->getUserId();
            successItem.moderatorLogin = account->getUserName();
            successItem.moderatorName = account->getUserName();
            successItem.updatedAt = QDateTime::currentDateTimeUtc();

            if (action == ReviewAction::Approve)
            {
                successItem.actionAccountID.clear();
                successItem.state = ReviewState::Approved;
                successItem.statusDetail = u"Approved from Leafyrino."_s;
                this->publish(successEntry, false);
                this->itemChanged.invoke(key);
                this->countsChanged.invoke();
                return;
            }

            successItem.state = ReviewState::Denied;
            if (action == ReviewAction::Deny)
            {
                successItem.actionAccountID.clear();
                successItem.statusDetail = u"Denied from Leafyrino."_s;
                this->publish(successEntry, false);
                this->itemChanged.invoke(key);
                this->countsChanged.invoke();
                return;
            }

            const auto secondary = action == ReviewAction::DenyAndBan
                                       ? ReviewAction::Ban
                                       : ReviewAction::Timeout;
            this->performSecondary(key, secondary, successItem.timeoutSeconds,
                                   successItem.moderationReason,
                                   account->getUserId());
        },
        [this, key, generation, action](HelixAutoModMessageError error) {
            this->handleDecisionFailure(key, generation, action, error);
        });
}

void AutoModReviewController::handleDecisionFailure(
    const QString &key, std::uint64_t generation, ReviewAction action,
    HelixAutoModMessageError error)
{
    auto it = this->entries_.find(key);
    if (it == this->entries_.end() || it->item.actionGeneration != generation)
    {
        return;
    }
    auto &entry = it.value();
    auto &item = entry.item;
    item.pendingAction = ReviewAction::None;
    item.failedAction = action;
    item.actionAccountID.clear();
    item.statusDetail = autoModErrorText(error);
    switch (error)
    {
        case HelixAutoModMessageError::MessageAlreadyProcessed:
            item.state = ReviewState::HandledElsewhere;
            item.failedAction = ReviewAction::None;
            break;
        case HelixAutoModMessageError::MessageNotFound:
            item.state = ReviewState::Expired;
            item.failedAction = ReviewAction::None;
            break;
        case HelixAutoModMessageError::UserNotAuthenticated:
        case HelixAutoModMessageError::UserNotAuthorized:
            item.state = ReviewState::PermissionLost;
            break;
        case HelixAutoModMessageError::Unknown:
        default:
            item.state = ReviewState::Failed;
            break;
    }
    item.updatedAt = QDateTime::currentDateTimeUtc();
    this->publish(entry, false);
    this->itemChanged.invoke(key);
    this->countsChanged.invoke();
}

void AutoModReviewController::performSecondary(
    const QString &key, ReviewAction action, int timeoutSeconds,
    const QString &reason, const QString &expectedModeratorID)
{
    auto it = this->entries_.find(key);
    if (it == this->entries_.end())
    {
        return;
    }
    auto &entry = it.value();
    auto &item = entry.item;
    this->updateActionsAllowed(entry);
    const auto account = getApp()->getAccounts()->twitch.getCurrent();

    const bool accountChanged =
        !expectedModeratorID.isEmpty() &&
        (entry.actionAccountGeneration != this->accountGeneration_ ||
         (account && !account->isAnon() &&
          account->getUserId() != expectedModeratorID));
    if (!item.actionsAllowed || !account || account->isAnon() || accountChanged)
    {
        item.state = ReviewState::Denied;
        item.pendingAction = ReviewAction::None;
        item.secondaryInProgress = false;
        item.secondaryFailed = true;
        item.secondaryAction = action;
        item.actionAccountID.clear();
        if (accountChanged)
        {
            item.secondaryError = u"Your Twitch account changed before the "_s %
                                  reviewActionLabel(action) % u" could run.";
        }
        else
        {
            item.secondaryError = u"The current Twitch account cannot "_s %
                                  reviewActionLabel(action) %
                                  u" users in this channel.";
        }
        item.statusDetail = u"Message denied. %1 failed: %2"_s.arg(
            reviewActionLabel(action), item.secondaryError);
        this->publish(entry, false);
        this->itemChanged.invoke(key);
        this->countsChanged.invoke();
        return;
    }

    item.secondaryInProgress = true;
    item.secondaryFailed = false;
    item.secondaryAction = action;
    item.secondaryError.clear();
    item.pendingAction = action;
    item.actionGeneration = ++this->actionGeneration_;
    const auto generation = item.actionGeneration;
    this->publish(entry, false);
    this->itemChanged.invoke(key);
    this->countsChanged.invoke();

    std::optional<int> duration;
    if (action == ReviewAction::Timeout)
    {
        duration = std::clamp(timeoutSeconds, 1, 14 * 24 * 60 * 60);
    }
    getHelix()->banUser(
        item.broadcasterID, account->getUserId(), item.userID, duration, reason,
        [this, key, generation, action, duration] {
            auto successIt = this->entries_.find(key);
            if (successIt == this->entries_.end() ||
                successIt->item.actionGeneration != generation)
            {
                return;
            }
            auto &successEntry = successIt.value();
            auto &successItem = successEntry.item;
            successItem.state = ReviewState::Denied;
            successItem.pendingAction = ReviewAction::None;
            successItem.secondaryInProgress = false;
            successItem.secondaryFailed = false;
            successItem.secondaryAction = action;
            successItem.actionAccountID.clear();
            successItem.secondaryError.clear();
            successItem.updatedAt = QDateTime::currentDateTimeUtc();
            if (action == ReviewAction::Ban)
            {
                successItem.statusDetail = u"Message denied and user banned."_s;
            }
            else
            {
                successItem.statusDetail =
                    u"Message denied and user timed out for %1."_s.arg(
                        formatTime(duration.value_or(0), 2));
            }
            this->publish(successEntry, false);
            this->itemChanged.invoke(key);
            this->countsChanged.invoke();
        },
        [this, key, generation, action](HelixBanUserError error,
                                        const QString &message) {
            this->handleSecondaryFailure(key, generation, action, error,
                                         message);
        });
}

void AutoModReviewController::handleSecondaryFailure(const QString &key,
                                                     std::uint64_t generation,
                                                     ReviewAction action,
                                                     HelixBanUserError error,
                                                     const QString &message)
{
    auto it = this->entries_.find(key);
    if (it == this->entries_.end() || it->item.actionGeneration != generation)
    {
        return;
    }
    auto &entry = it.value();
    auto &item = entry.item;
    item.state = ReviewState::Denied;
    item.pendingAction = ReviewAction::None;
    item.secondaryInProgress = false;
    item.secondaryFailed = true;
    item.secondaryAction = action;
    item.actionAccountID.clear();
    item.secondaryError = punishmentErrorText(error, message);
    if (error == HelixBanUserError::TargetBanned)
    {
        item.secondaryFailed = false;
        item.secondaryError.clear();
        item.statusDetail = u"Message denied. The user was already banned."_s;
        item.updatedAt = QDateTime::currentDateTimeUtc();
        this->publish(entry, false);
        this->itemChanged.invoke(key);
        this->countsChanged.invoke();
        return;
    }
    item.statusDetail = u"Message denied. %1 failed: %2"_s.arg(
        reviewActionLabel(action), item.secondaryError);
    item.updatedAt = QDateTime::currentDateTimeUtc();
    this->publish(entry, false);
    this->itemChanged.invoke(key);
    this->countsChanged.invoke();
}

void AutoModReviewController::retry(const QString &key)
{
    assertInGuiThread();
    auto it = this->entries_.find(key);
    if (it == this->entries_.end())
    {
        return;
    }
    const auto item = it->item;
    if (item.secondaryFailed &&
        (item.secondaryAction == ReviewAction::Timeout ||
         item.secondaryAction == ReviewAction::Ban))
    {
        this->performSecondary(key, item.secondaryAction, item.timeoutSeconds,
                               item.moderationReason);
        return;
    }
    if (item.failedAction != ReviewAction::None)
    {
        this->performDecision(key, item.failedAction, item.timeoutSeconds,
                              item.moderationReason, true);
    }
}

const ReviewItem *AutoModReviewController::find(const QString &key) const
{
    const auto it = this->entries_.constFind(key);
    return it == this->entries_.cend() ? nullptr : &it->item;
}

QVector<const ReviewItem *> AutoModReviewController::items() const
{
    QVector<const ReviewItem *> result;
    result.reserve(this->order_.size());
    for (const auto &key : this->order_)
    {
        if (const auto *item = this->find(key))
        {
            result.push_back(item);
        }
    }
    return result;
}

ActiveReviewSummary AutoModReviewController::activeSummary() const
{
    ActiveReviewSummary result;
    QHash<QString, int> channelCounts;
    for (const auto *item : this->items())
    {
        if (reviewGroup(*item) == ReviewGroup::Resolved)
        {
            continue;
        }

        ++result.total;
        if (!item->broadcasterLogin.isEmpty())
        {
            ++channelCounts[item->broadcasterLogin.toLower()];
        }
    }

    result.channels.reserve(channelCounts.size());
    for (auto it = channelCounts.cbegin(); it != channelCounts.cend(); ++it)
    {
        result.channels.push_back({it.key(), it.value()});
    }
    std::ranges::sort(result.channels, {}, &ActiveChannelCount::login);
    return result;
}

std::pair<MessagePtr, HighlightAlert> AutoModReviewController::publish(
    Entry &entry, bool addIfMissing)
{
    auto source = entry.sourceChannel.lock();
    this->updateActionsAllowed(entry);
    const auto chatPresentation =
        getSettings()->advancedAutoModInChat
            ? eventsub::AutoModReviewPresentation::ChatAdvanced
            : eventsub::AutoModReviewPresentation::ChatLegacy;
    auto [chatMessage, alert] = eventsub::makeAutoModReviewMessage(
        source.get(), entry.item, chatPresentation);
    auto queueMessage = eventsub::makeAutoModReviewMessage(
                            source.get(), entry.item,
                            eventsub::AutoModReviewPresentation::ReviewQueue)
                            .first;
    const auto id = autoModMessageID(entry.item);

    // the highlight decides if a new review goes to /mentions, keep that
    // choice for the rest of its life
    if (addIfMissing && chatMessage->flags.has(MessageFlag::ShowInMentions))
    {
        entry.item.shownInMentions = true;
    }

    const auto publishTo = [&](const std::shared_ptr<Channel> &channel,
                               const MessagePtr &message, bool mayAdd) {
        if (!channel || channel->isEmpty())
        {
            return;
        }
        if (const auto existing = channel->findMessageByID(id))
        {
            channel->replaceMessage(existing, message);
        }
        else if (mayAdd)
        {
            channel->addMessage(message, MessageContext::Original);
        }
    };

    publishTo(source, chatMessage, addIfMissing);
    publishTo(getApp()->getTwitch()->getAutomodChannel(), queueMessage,
              addIfMissing);
    if (entry.item.shownInMentions)
    {
        publishTo(getApp()->getTwitch()->getMentionsChannel(), chatMessage,
                  addIfMissing);
    }

    return {chatMessage, alert};
}

void AutoModReviewController::watchChannelPermissions(
    const QString &broadcasterID,
    const std::shared_ptr<TwitchChannel> &sourceChannel)
{
    if (!sourceChannel)
    {
        return;
    }
    const auto key = broadcasterID.isEmpty()
                         ? sourceChannel->getName().toLower()
                         : broadcasterID;
    if (const auto existing = this->permissionChannels_.value(key).lock();
        existing == sourceChannel)
    {
        return;
    }

    this->permissionChannels_.insert(key, sourceChannel);
    this->signalHolder_.managedConnect(sourceChannel->userStateChanged, [this] {
        this->refreshPermissions();
    });
}

void AutoModReviewController::refreshPermissions()
{
    assertInGuiThread();
    for (auto it = this->entries_.begin(); it != this->entries_.end(); ++it)
    {
        const auto previous = it->item.actionsAllowed;
        this->updateActionsAllowed(it.value());
        if (previous != it->item.actionsAllowed)
        {
            this->publish(it.value(), false);
            this->itemChanged.invoke(it.key());
        }
    }
    this->countsChanged.invoke();
}

void AutoModReviewController::updateActionsAllowed(Entry &entry)
{
    const auto account = getApp()->getAccounts()->twitch.getCurrent();
    const auto channel = entry.sourceChannel.lock();
    entry.item.actionsAllowed =
        account && !account->isAnon() && channel && channel->hasModRights();
}

void AutoModReviewController::ensureContext(const QString &key)
{
    assertInGuiThread();
    auto it = this->entries_.find(key);
    if (it == this->entries_.end() || it->item.contextCaptured)
    {
        return;
    }

    auto &entry = it.value();
    entry.item.contextCaptured = true;
    this->captureContext(entry);
    if (entry.item.context.isEmpty())
    {
        return;
    }

    this->publish(entry, false);
    this->itemChanged.invoke(key);
}

void AutoModReviewController::captureContext(Entry &entry)
{
    const auto channel = entry.sourceChannel.lock();
    if (!channel)
    {
        return;
    }

    const auto snapshot = channel->getMessageSnapshot(CONTEXT_SCAN_LIMIT);
    for (auto it = snapshot.crbegin();
         it != snapshot.crend() &&
         entry.item.context.size() < CONTEXT_MESSAGE_LIMIT;
         ++it)
    {
        const auto &message = *it;
        if (!message || message->flags.hasAny({MessageFlag::System,
                                               MessageFlag::ModerationAction,
                                               MessageFlag::AutoMod}))
        {
            continue;
        }
        const bool sameUser =
            (!entry.item.userID.isEmpty() &&
             message->userID == entry.item.userID) ||
            (!entry.item.userLogin.isEmpty() &&
             message->loginName.compare(entry.item.userLogin,
                                        Qt::CaseInsensitive) == 0);
        if (!sameUser || message->messageText.isEmpty())
        {
            continue;
        }
        if (entry.item.receivedAt.isValid() &&
            message->serverReceivedTime.isValid() &&
            message->serverReceivedTime >= entry.item.receivedAt)
        {
            continue;
        }
        entry.item.context.push_back({
            .displayName = message->displayName.isEmpty()
                               ? message->loginName
                               : message->displayName,
            .text = message->messageText,
            .receivedAt = message->serverReceivedTime,
        });
    }
    std::reverse(entry.item.context.begin(), entry.item.context.end());
}

void AutoModReviewController::prune()
{
    while (this->order_.size() > MAX_REVIEW_ITEMS)
    {
        auto removeIndex = -1;
        for (qsizetype i = 0; i < this->order_.size(); ++i)
        {
            const auto *item = this->find(this->order_.at(i));
            if (item != nullptr && reviewGroup(*item) == ReviewGroup::Resolved)
            {
                removeIndex = static_cast<int>(i);
                break;
            }
        }
        if (removeIndex < 0)
        {
            removeIndex = 0;
        }
        const auto key = this->order_.takeAt(removeIndex);
        this->entries_.remove(key);
        this->itemRemoved.invoke(key);
    }
}

}  // namespace chatterino::automod

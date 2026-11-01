#pragma once

#include "controllers/highlights/HighlightMatch.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/twitch/api/HelixEnums.hpp"

#include <boost/signals2/connection.hpp>
#include <pajlada/signals.hpp>
#include <QDateTime>
#include <QHash>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace chatterino {

class Channel;
class TwitchChannel;

namespace automod {

enum class ReviewState : std::uint8_t {
    Open,
    Approving,
    Denying,
    Approved,
    Denied,
    HandledElsewhere,
    Expired,
    Failed,
    PermissionLost,
};

enum class ReviewAction : std::uint8_t {
    None,
    Approve,
    Deny,
    DenyAndTimeout,
    DenyAndBan,
    Timeout,
    Ban,
};

enum class ReviewGroup : std::uint8_t {
    Open,
    Failed,
    Resolved,
};

struct ContextMessage {
    QString displayName;
    QString text;
    QDateTime receivedAt;
};

struct HoldData {
    QString notificationID;
    QString broadcasterID;
    QString broadcasterLogin;
    QString broadcasterName;
    QString userID;
    QString userLogin;
    QString userName;
    QString messageID;
    QString messageText;
    QDateTime receivedAt;

    bool blockedTerm = false;
    QString reasonSummary;
    QString reasonCategory;
    int reasonLevel = 0;
    QStringList matchedFragments;
    QString termOwnerLogin;
    std::vector<HighlightMatch> twitchMatches;
};

struct UpdateData : HoldData {
    QString status;
    QString moderatorID;
    QString moderatorLogin;
    QString moderatorName;
};

struct ReviewItem {
    QString key;
    QString broadcasterID;
    QString broadcasterLogin;
    QString broadcasterName;
    QString userID;
    QString userLogin;
    QString userName;
    QString messageID;
    QString messageText;
    QDateTime receivedAt;
    QDateTime updatedAt;

    bool blockedTerm = false;
    QString reasonSummary;
    QString reasonCategory;
    int reasonLevel = 0;
    QStringList matchedFragments;
    QString termOwnerLogin;

    ReviewState state = ReviewState::Open;
    ReviewAction pendingAction = ReviewAction::None;
    ReviewAction failedAction = ReviewAction::None;
    QString statusDetail;
    QString moderatorID;
    QString moderatorLogin;
    QString moderatorName;
    QString actionAccountID;

    bool actionsAllowed = true;
    bool shownInMentions = false;
    bool secondaryInProgress = false;
    bool secondaryFailed = false;
    ReviewAction secondaryAction = ReviewAction::None;
    int timeoutSeconds = 0;
    QString moderationReason;
    QString secondaryError;

    QVector<ContextMessage> context;
    bool contextCaptured = false;
    std::shared_ptr<const std::vector<HighlightMatch>> highlightMatches;

    std::uint64_t actionGeneration = 0;
};

struct ActiveChannelCount {
    QString login;
    int count = 0;
};

struct ActiveReviewSummary {
    int total = 0;
    QVector<ActiveChannelCount> channels;
};

ReviewGroup reviewGroup(const ReviewItem &item);
bool isActionable(const ReviewItem &item);
QString reviewStateLabel(const ReviewItem &item);
QString reviewActionLabel(ReviewAction action);

class AutoModReviewController
{
public:
    AutoModReviewController();
    ~AutoModReviewController();

    AutoModReviewController(const AutoModReviewController &) = delete;
    AutoModReviewController(AutoModReviewController &&) = delete;
    AutoModReviewController &operator=(const AutoModReviewController &) =
        delete;
    AutoModReviewController &operator=(AutoModReviewController &&) = delete;

    void initialize();

    static QString makeKey(QStringView broadcasterID, QStringView messageID);

    void ingestHold(HoldData data,
                    const std::shared_ptr<TwitchChannel> &sourceChannel);
    void ingestUpdate(UpdateData data,
                      const std::shared_ptr<TwitchChannel> &sourceChannel);

    void approve(const QString &key);
    void deny(const QString &key);
    void denyAndTimeout(const QString &key, int durationSeconds,
                        QString reason);
    void denyAndBan(const QString &key, QString reason);
    void retry(const QString &key);
    void ensureContext(const QString &key);

    const ReviewItem *find(const QString &key) const;
    QVector<const ReviewItem *> items() const;
    ActiveReviewSummary activeSummary() const;

    pajlada::Signals::Signal<const QString &> itemAdded;
    pajlada::Signals::Signal<const QString &> itemChanged;
    pajlada::Signals::Signal<const QString &> itemRemoved;
    pajlada::Signals::NoArgSignal countsChanged;

private:
    struct Entry {
        ReviewItem item;
        std::weak_ptr<TwitchChannel> sourceChannel;
        std::uint64_t actionAccountGeneration = 0;
    };

    void performDecision(const QString &key, ReviewAction action,
                         int timeoutSeconds = 0, QString reason = {},
                         bool retrying = false);
    void performSecondary(const QString &key, ReviewAction action,
                          int timeoutSeconds, const QString &reason,
                          const QString &expectedModeratorID = {});
    void handleDecisionFailure(const QString &key, std::uint64_t generation,
                               ReviewAction action,
                               HelixAutoModMessageError error);
    void handleSecondaryFailure(const QString &key, std::uint64_t generation,
                                ReviewAction action, HelixBanUserError error,
                                const QString &message);

    /// Rebuilds the entry's messages and replaces them in the source channel,
    /// /automod and /mentions. Returns the chat message and its highlight
    /// alert so the caller can trigger it once for a new hold.
    std::pair<MessagePtr, HighlightAlert> publish(Entry &entry,
                                                  bool addIfMissing);
    void watchChannelPermissions(
        const QString &broadcasterID,
        const std::shared_ptr<TwitchChannel> &sourceChannel);
    void refreshPermissions();
    void updateActionsAllowed(Entry &entry);
    void captureContext(Entry &entry);
    void prune();
    bool rememberNotification(const QString &notificationID);

    QHash<QString, Entry> entries_;
    QVector<QString> order_;
    QSet<QString> notificationIDs_;
    QQueue<QString> notificationOrder_;
    QHash<QString, std::weak_ptr<TwitchChannel>> permissionChannels_;
    std::uint64_t accountGeneration_ = 0;

    std::uint64_t actionGeneration_ = 0;
    pajlada::Signals::SignalHolder signalHolder_;
    boost::signals2::scoped_connection accountConnection_;
};

}  // namespace automod
}  // namespace chatterino

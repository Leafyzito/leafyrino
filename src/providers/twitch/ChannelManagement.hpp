#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace chatterino {

class TwitchChannel;

struct ChannelManagementCategory {
    QString id;
    QString name;
    QString displayName;
    QString boxArtUrl;
};

struct ChannelManagementContentLabel {
    QString id;
    QString name;
    QString description;
    QString lockedUntil;
    bool isEnabled = false;
    bool isLocked = false;
    bool isSelectable = true;
};

struct ChannelManagementMetadata {
    QString channelId;
    QString title;
    QString language;
    ChannelManagementCategory category;
    QStringList tags;
    std::vector<ChannelManagementContentLabel> contentLabels;
    bool isRerun = false;
    bool canEditRerun = false;
    QString audience = QStringLiteral("Everyone");
    bool canEditAudience = false;
};

struct ChannelManagementUpdate {
    std::optional<QString> title;
    std::optional<QString> language;
    std::optional<ChannelManagementCategory> category;
    std::optional<QStringList> tags;
    std::optional<std::vector<ChannelManagementContentLabel>> contentLabels;
    std::optional<bool> isRerun;

    [[nodiscard]] bool empty() const
    {
        return !this->title && !this->language && !this->category &&
               !this->tags && !this->contentLabels && !this->isRerun;
    }
};

enum class ChannelManagementAccess {
    Broadcaster,
    Editor,
};

struct ChannelManagementState {
    ChannelManagementAccess access = ChannelManagementAccess::Broadcaster;
    ChannelManagementMetadata metadata;
};

enum class ChannelManagementCommercialTrigger {
    ChatCommand,
    QuickAction,
};

struct ChannelManagementCommercialResult {
    int lengthSeconds = 0;
    int retryAfterSeconds = 0;
};

struct ChannelManagementCommercialFailure {
    QString message;
    int retryAfterSeconds = 0;
};

namespace ChannelManagement {

using FailureCallback = std::function<void(const QString &)>;

bool isValidCommercialLength(int lengthSeconds);
bool isValidTag(const QString &tag);
bool hasVerifiedEditorAccess(const QString &channelId);
int commercialCooldownRemainingSeconds(const QString &channelId);
void rankCategoryResults(std::vector<ChannelManagementCategory> &categories,
                         const QString &query);

void verifyAccess(const std::shared_ptr<TwitchChannel> &channel,
                  bool forceEditorRefresh,
                  std::function<void(ChannelManagementAccess)> successCallback,
                  FailureCallback failureCallback);

void loadState(const std::shared_ptr<TwitchChannel> &channel,
               std::function<void(ChannelManagementState)> successCallback,
               FailureCallback failureCallback);

void searchCategories(
    const QString &query,
    std::function<void(std::vector<ChannelManagementCategory>)> successCallback,
    FailureCallback failureCallback);

void updateMetadata(const std::shared_ptr<TwitchChannel> &channel,
                    ChannelManagementUpdate update,
                    std::function<void()> successCallback,
                    FailureCallback failureCallback);

void updateMetadata(const std::shared_ptr<TwitchChannel> &channel,
                    std::optional<QString> title,
                    std::optional<ChannelManagementCategory> category,
                    std::function<void()> successCallback,
                    FailureCallback failureCallback);

void updateTitle(const std::shared_ptr<TwitchChannel> &channel,
                 const QString &title, std::function<void()> successCallback,
                 FailureCallback failureCallback);

void updateCategory(const std::shared_ptr<TwitchChannel> &channel,
                    const ChannelManagementCategory &category,
                    std::function<void()> successCallback,
                    FailureCallback failureCallback);

void startCommercial(
    const std::shared_ptr<TwitchChannel> &channel, int lengthSeconds,
    ChannelManagementCommercialTrigger trigger,
    std::function<void(ChannelManagementCommercialResult)> successCallback,
    std::function<void(ChannelManagementCommercialFailure)> failureCallback);

}  // namespace ChannelManagement

}  // namespace chatterino

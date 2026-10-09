// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/ChatterListWidget.hpp"

#include "Application.hpp"
#include "common/ChannelChatters.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "messages/Message.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "util/Helpers.hpp"
#include "util/MultiChannel.hpp"

#include <pajlada/signals/signal.hpp>
#include <QAbstractListModel>
#include <QCoreApplication>
#include <QDateTime>
#include <QHash>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QStringList>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace chatterino {

namespace {

constexpr auto COMMUNITY_ENDPOINT = "https://api.tackling.cc/twitch/Chatters";
constexpr auto CHATTER_LIMIT = 20000;
constexpr auto COMMUNITY_CACHE_TTL_MS = 5 * 60 * 1000;
constexpr auto COMMUNITY_CACHE_LIMIT = 5;
constexpr auto COMMUNITY_MAX_RESPONSE_BYTES = 4 * 1024 * 1024;
constexpr auto COMMUNITY_REQUEST_TIMEOUT_MS = 15 * 1000;
constexpr auto COMMUNITY_RETRY_BASE_DELAY_MS = 4 * 1000;
constexpr auto COMMUNITY_REQUEST_SPACING_MS = 1500;
constexpr auto COMMUNITY_REQUEST_ATTEMPTS = 3;
constexpr auto TWITCH_CHATTER_LIMIT = 5000;
constexpr auto TWITCH_MAX_CONCURRENT_REQUESTS = 2;

enum class ChatterRole : std::uint8_t {
    Broadcaster,
    Staff,
    ChatBot,
    Moderator,
    Vip,
    Viewer,
};

enum RoleFlag : std::uint8_t {
    NoRole = 0,
    BroadcasterRole = 1 << 0,
    StaffRole = 1 << 1,
    ChatBotRole = 1 << 2,
    ModeratorRole = 1 << 3,
    VipRole = 1 << 4,
};

enum class SourceState : std::uint8_t {
    Skipped,
    Pending,
    Ready,
    Cached,
    Partial,
    Failed,
};

enum ModelRole {
    RowKindRole = Qt::UserRole + 1,
    LoginRole,
    DisplayNameRole,
    GroupCountRole,
    PlatformRole,
    ChannelNameRole,
    ChannelDisplayNameRole,
    UserIdRole,
    PlatformNameRole,
};

enum class RowKind : std::uint8_t {
    Group,
    User,
    Status,
};

struct ChatterRecord {
    QString login;
    QString displayName;
    QString channelName;
    QString channelDisplayName;
    QString userId;
    MessagePlatform platform = MessagePlatform::AnyOrTwitch;
    std::uint8_t roles = NoRole;
    int displayPriority = 0;
};

struct ChatterRow {
    RowKind kind = RowKind::Status;
    QString key;
    QString text;
    int count = 0;
};

struct CommunityUser {
    QString login;
    std::uint8_t roles = NoRole;
};

struct CommunityPayload {
    std::vector<CommunityUser> users;
    int reportedCount = 0;
};

struct CommunityWorkItem {
    QString channelName;
    QString channelDisplayName;
    int mergeLimit = 0;
};

using CommunityCallback =
    std::function<void(std::shared_ptr<const CommunityPayload>, bool)>;

struct CommunityCacheEntry {
    std::shared_ptr<const CommunityPayload> payload;
    qint64 expiresAt = 0;
    qint64 lastUsedAt = 0;
};

struct CommunityCacheState {
    std::mutex mutex;
    QHash<QString, CommunityCacheEntry> entries;
    QHash<QString, std::vector<CommunityCallback>> waiters;
};

struct CommunityRequestJob {
    QString login;
    int attempt = 0;
};

struct CommunityRequestScheduler {
    std::deque<CommunityRequestJob> queue;
    bool active = false;
};

CommunityCacheState &communityCache()
{
    static CommunityCacheState cache;
    return cache;
}

CommunityRequestScheduler &communityRequestScheduler()
{
    static CommunityRequestScheduler scheduler;
    return scheduler;
}

void pruneCommunityCache(CommunityCacheState &cache, qint64 now)
{
    for (auto it = cache.entries.begin(); it != cache.entries.end();)
    {
        if (it->expiresAt <= now)
        {
            it = cache.entries.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

QString normalizedLogin(QString login)
{
    login = login.trimmed().toLower();
    if (login.isEmpty() || login.size() > 25)
    {
        return {};
    }

    for (const auto character : login)
    {
        const auto isAsciiLetter =
            (character >= QLatin1Char('a') && character <= QLatin1Char('z'));
        const auto isAsciiDigit =
            (character >= QLatin1Char('0') && character <= QLatin1Char('9'));
        if (!(isAsciiLetter || isAsciiDigit || character == QLatin1Char('_')))
        {
            return {};
        }
    }
    return login;
}

QString chatterKey(const QString &login, MessagePlatform platform,
                   const QString &channelName, const QString &userId = {})
{
    Q_UNUSED(userId);
    auto identity = login.trimmed().toCaseFolded();
    auto identityKind = QLatin1String("name");
    if (identity.isEmpty() || identity.size() > 256)
    {
        return {};
    }
    auto key = QString::number(static_cast<int>(platform));
    key += QChar(0);
    const auto source = channelName.trimmed();
    key += source.toCaseFolded();
    key += QChar(0);
    key += identityKind;
    key += QChar(0);
    key += identity;
    return key;
}

QString channelKey(MessagePlatform platform, const QString &channelName)
{
    const auto source = channelName.trimmed();
    return QStringLiteral("%1:%2")
        .arg(static_cast<int>(platform))
        .arg(source.toCaseFolded());
}

QString platformName(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return QStringLiteral("Kick");
        case MessagePlatform::YouTube:
            return {};
        case MessagePlatform::AnyOrTwitch:
            return QStringLiteral("Twitch");
    }
    return {};
}

ChatterRole primaryRole(std::uint8_t roles)
{
    if (roles & BroadcasterRole)
    {
        return ChatterRole::Broadcaster;
    }
    if (roles & StaffRole)
    {
        return ChatterRole::Staff;
    }
    if (roles & ChatBotRole)
    {
        return ChatterRole::ChatBot;
    }
    if (roles & ModeratorRole)
    {
        return ChatterRole::Moderator;
    }
    if (roles & VipRole)
    {
        return ChatterRole::Vip;
    }
    return ChatterRole::Viewer;
}

QString groupName(ChatterRole role)
{
    switch (role)
    {
        case ChatterRole::Broadcaster:
            return QStringLiteral("Broadcaster");
        case ChatterRole::Staff:
            return QStringLiteral("Staff");
        case ChatterRole::ChatBot:
            return QStringLiteral("Chat bots");
        case ChatterRole::Moderator:
            return QStringLiteral("Moderators");
        case ChatterRole::Vip:
            return QStringLiteral("VIPs");
        case ChatterRole::Viewer:
            return QStringLiteral("Chatters");
    }
    return {};
}

std::uint8_t rolesFromMessage(const Message &message, bool useBadges)
{
    if (!useBadges)
    {
        return NoRole;
    }

    std::uint8_t roles = NoRole;
    for (const auto &badge : message.twitchBadges)
    {
        if (badge.key_ == QStringLiteral("broadcaster"))
        {
            roles |= BroadcasterRole;
        }
        else if (badge.key_ == QStringLiteral("staff"))
        {
            roles |= StaffRole;
        }
        else if (badge.key_ == QStringLiteral("moderator") ||
                 badge.key_ == QStringLiteral("lead_moderator"))
        {
            roles |= ModeratorRole;
        }
        else if (badge.key_ == QStringLiteral("vip"))
        {
            roles |= VipRole;
        }
    }
    return roles;
}

std::shared_ptr<const CommunityPayload> parseCommunityPayload(
    const NetworkResult &result)
{
    if (result.status() != 200 ||
        result.getData().size() > COMMUNITY_MAX_RESPONSE_BYTES)
    {
        return {};
    }

    const auto root = result.parseJson();
    if (!root.contains(QStringLiteral("viewers")) &&
        !root.contains(QStringLiteral("broadcasters")))
    {
        return {};
    }

    QHash<QString, std::uint8_t> users;
    users.reserve(std::clamp(root.value(QStringLiteral("count")).toInt(), 0,
                             CHATTER_LIMIT));

    const auto addGroup = [&root, &users](const char *name, std::uint8_t role) {
        const auto values = root.value(QLatin1String(name)).toArray();
        for (const auto &value : values)
        {
            if (!value.isString())
            {
                continue;
            }

            const auto login = normalizedLogin(value.toString());
            if (login.isEmpty())
            {
                continue;
            }

            auto it = users.find(login);
            if (it != users.end())
            {
                it.value() |= role;
                continue;
            }
            if (users.size() >= CHATTER_LIMIT)
            {
                continue;
            }
            users.insert(login, role);
        }
    };

    addGroup("broadcasters", BroadcasterRole);
    addGroup("staff", StaffRole);
    addGroup("moderators", ModeratorRole);
    addGroup("vips", VipRole);
    addGroup("chatbots", ChatBotRole);
    addGroup("viewers", NoRole);

    if (users.isEmpty())
    {
        return {};
    }

    auto payload = std::make_shared<CommunityPayload>();
    payload->reportedCount =
        std::max(static_cast<int>(users.size()),
                 std::max(0, root.value(QStringLiteral("count")).toInt()));
    payload->users.reserve(static_cast<size_t>(users.size()));
    for (auto it = users.cbegin(); it != users.cend(); ++it)
    {
        payload->users.push_back({it.key(), it.value()});
    }
    std::sort(payload->users.begin(), payload->users.end(),
              [](const CommunityUser &left, const CommunityUser &right) {
                  const auto leftRole = primaryRole(left.roles);
                  const auto rightRole = primaryRole(right.roles);
                  if (leftRole != rightRole)
                  {
                      return leftRole < rightRole;
                  }
                  return QString::compare(left.login, right.login,
                                          Qt::CaseInsensitive) < 0;
              });
    return payload;
}

void finishCommunityRequest(const QString &login,
                            std::shared_ptr<const CommunityPayload> payload)
{
    std::vector<CommunityCallback> waiters;
    std::shared_ptr<const CommunityPayload> fallbackPayload;
    auto &cache = communityCache();
    const auto now = QDateTime::currentMSecsSinceEpoch();
    {
        const std::scoped_lock lock(cache.mutex);
        waiters = std::move(cache.waiters[login]);
        cache.waiters.remove(login);

        auto existing = cache.entries.find(login);
        const auto hasUsableExisting = existing != cache.entries.end() &&
                                       existing->payload &&
                                       existing->expiresAt > now;
        if (payload)
        {
            cache.entries.insert(login,
                                 {payload, now + COMMUNITY_CACHE_TTL_MS, now});
        }
        else if (hasUsableExisting)
        {
            existing->lastUsedAt = now;
            fallbackPayload = existing->payload;
        }
        else if (existing != cache.entries.end())
        {
            cache.entries.erase(existing);
        }

        while (cache.entries.size() > COMMUNITY_CACHE_LIMIT)
        {
            const auto oldest =
                std::min_element(cache.entries.begin(), cache.entries.end(),
                                 [](const auto &left, const auto &right) {
                                     return left.lastUsedAt < right.lastUsedAt;
                                 });
            cache.entries.erase(oldest);
        }
    }

    for (auto &callback : waiters)
    {
        callback(payload ? payload : fallbackPayload,
                 !payload && fallbackPayload != nullptr);
    }

    if ((payload || fallbackPayload) && QCoreApplication::instance() != nullptr)
    {
        QTimer::singleShot(
            COMMUNITY_CACHE_TTL_MS, QCoreApplication::instance(), [] {
                auto &currentCache = communityCache();
                const std::scoped_lock lock(currentCache.mutex);
                pruneCommunityCache(currentCache,
                                    QDateTime::currentMSecsSinceEpoch());
            });
    }
}

void startCommunityRequest(const QString &login, int attempt);

void pumpCommunityRequestQueue();

void releaseCommunityRequestLane()
{
    auto *application = QCoreApplication::instance();
    if (application == nullptr)
    {
        return;
    }
    QTimer::singleShot(COMMUNITY_REQUEST_SPACING_MS, application, [] {
        communityRequestScheduler().active = false;
        pumpCommunityRequestQueue();
    });
}

void completeCommunityRequest(const QString &login,
                              std::shared_ptr<const CommunityPayload> payload)
{
    finishCommunityRequest(login, std::move(payload));

    releaseCommunityRequestLane();
}

void retryOrFinishCommunityRequest(const QString &login, int attempt)
{
    if (attempt + 1 < COMMUNITY_REQUEST_ATTEMPTS)
    {
        if (auto *application = QCoreApplication::instance())
        {
            const auto delay = COMMUNITY_RETRY_BASE_DELAY_MS * (attempt + 1);
            releaseCommunityRequestLane();
            QTimer::singleShot(delay, application, [login, attempt] {
                auto &scheduler = communityRequestScheduler();
                scheduler.queue.push_back({login, attempt + 1});
                pumpCommunityRequestQueue();
            });
            return;
        }
    }
    completeCommunityRequest(login, {});
}

void startCommunityRequest(const QString &login, int attempt)
{
    const auto attemptLimit = CHATTER_LIMIT - attempt;
    QUrl url(QString::fromLatin1(COMMUNITY_ENDPOINT));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("login"), login);
    query.addQueryItem(QStringLiteral("limit"), QString::number(attemptLimit));
    url.setQuery(query);

    NetworkRequest(url)
        .timeout(COMMUNITY_REQUEST_TIMEOUT_MS)
        .header("Accept", "application/json")
        .onSuccess([login, attempt](const NetworkResult &result) {
            auto payload = parseCommunityPayload(result);
            if (payload)
            {
                completeCommunityRequest(login, std::move(payload));
            }
            else
            {
                qCWarning(chatterinoHTTP)
                    << "Extended chatter list returned an unusable response for"
                    << login << "on attempt" << attempt + 1 << "with"
                    << result.getData().size() << "bytes";
                retryOrFinishCommunityRequest(login, attempt);
            }
        })
        .onError([login, attempt](const NetworkResult &result) {
            const auto status = result.status();
            const auto permanentClientError =
                status >= 400 && status < 500 && status != 408 && status != 429;
            if (permanentClientError)
            {
                qCWarning(chatterinoHTTP)
                    << "Extended chatter list request failed permanently for"
                    << login << result.formatError();
                completeCommunityRequest(login, {});
            }
            else
            {
                qCWarning(chatterinoHTTP)
                    << "Extended chatter list request failed for" << login
                    << "on attempt" << attempt + 1 << result.formatError();
                retryOrFinishCommunityRequest(login, attempt);
            }
        })
        .execute();
}

void pumpCommunityRequestQueue()
{
    auto &scheduler = communityRequestScheduler();
    if (scheduler.active || scheduler.queue.empty())
    {
        return;
    }

    auto job = std::move(scheduler.queue.front());
    scheduler.queue.pop_front();
    scheduler.active = true;
    startCommunityRequest(job.login, job.attempt);
}

void enqueueCommunityRequest(const QString &login)
{
    auto &scheduler = communityRequestScheduler();
    scheduler.queue.push_back({login, 0});
    pumpCommunityRequestQueue();
}

void requestCommunityChatters(const QString &login, bool force,
                              CommunityCallback callback)
{
    const auto channelLogin = normalizedLogin(login);
    if (channelLogin.isEmpty())
    {
        callback({}, false);
        return;
    }

    auto &cache = communityCache();
    const auto now = QDateTime::currentMSecsSinceEpoch();
    std::shared_ptr<const CommunityPayload> cached;
    bool hasCachedResponse = false;
    bool startRequest = false;

    {
        const std::scoped_lock lock(cache.mutex);
        pruneCommunityCache(cache, now);

        if (!force)
        {
            auto it = cache.entries.find(channelLogin);
            if (it != cache.entries.end() && it->payload)
            {
                it->lastUsedAt = now;
                cached = it->payload;
                hasCachedResponse = true;
            }
        }

        if (!hasCachedResponse)
        {
            auto &waiters = cache.waiters[channelLogin];
            startRequest = waiters.empty();
            waiters.push_back(std::move(callback));
        }
    }

    if (hasCachedResponse)
    {
        callback(std::move(cached), true);
        return;
    }
    if (!startRequest)
    {
        return;
    }

    enqueueCommunityRequest(channelLogin);
}

class ChatterListModel final : public QAbstractListModel
{
public:
    explicit ChatterListModel(QObject *parent)
        : QAbstractListModel(parent)
    {
    }

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : this->rows_.size();
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 ||
            index.row() >= this->rows_.size())
        {
            return {};
        }

        const auto &row = this->rows_.at(index.row());
        if (role == RowKindRole)
        {
            return static_cast<int>(row.kind);
        }
        if (role == GroupCountRole)
        {
            return row.count;
        }
        if (row.kind != RowKind::User)
        {
            return role == Qt::DisplayRole ? row.text : QVariant{};
        }

        const auto it = this->chatters_.constFind(row.key);
        if (it == this->chatters_.cend())
        {
            return {};
        }
        if (role == Qt::DisplayRole || role == DisplayNameRole)
        {
            return it->displayName;
        }
        if (role == LoginRole)
        {
            return it->login;
        }
        if (role == PlatformRole)
        {
            return static_cast<int>(it->platform);
        }
        if (role == ChannelNameRole)
        {
            return it->channelName;
        }
        if (role == ChannelDisplayNameRole)
        {
            return it->channelDisplayName;
        }
        if (role == UserIdRole)
        {
            return it->userId;
        }
        if (role == PlatformNameRole)
        {
            return platformName(it->platform);
        }
        if (role == Qt::ToolTipRole)
        {
            return QStringLiteral("@%1").arg(it->login);
        }
        return {};
    }

    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        if (!index.isValid() || index.row() < 0 ||
            index.row() >= this->rows_.size() ||
            this->rows_.at(index.row()).kind != RowKind::User)
        {
            return Qt::NoItemFlags;
        }
        const auto chatter =
            this->chatters_.constFind(this->rows_.at(index.row()).key);
        if (chatter == this->chatters_.cend() ||
            chatter->platform == MessagePlatform::YouTube)
        {
            return Qt::NoItemFlags;
        }
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    }

    void clear()
    {
        this->beginResetModel();
        this->chatters_.clear();
        this->rows_.clear();
        this->endResetModel();
    }

    void reserve(int expectedSize)
    {
        this->chatters_.reserve(
            std::clamp(expectedSize, static_cast<int>(this->chatters_.size()),
                       CHATTER_LIMIT));
    }

    bool add(const QString &login, const QString &displayName,
             std::uint8_t roles, int displayPriority, MessagePlatform platform,
             const QString &channelName, const QString &channelDisplayName,
             const QString &userId = {})
    {
        auto key = chatterKey(login, platform, channelName, userId);
        if (key.isEmpty())
        {
            return false;
        }

        auto it = this->chatters_.find(key);
        if (it == this->chatters_.end())
        {
            if (this->chatters_.size() >= CHATTER_LIMIT)
            {
                if (roles == NoRole)
                {
                    return false;
                }
                auto viewer =
                    std::find_if(this->chatters_.begin(), this->chatters_.end(),
                                 [](const ChatterRecord &record) {
                                     return record.roles == NoRole;
                                 });
                if (viewer == this->chatters_.end())
                {
                    return false;
                }
                this->chatters_.erase(viewer);
            }
            ChatterRecord record;
            record.login = login.trimmed();
            record.displayName = displayName.trimmed().isEmpty()
                                     ? login.trimmed()
                                     : displayName.trimmed();
            if (record.displayName.isEmpty())
            {
                record.displayName = key;
            }
            record.channelName = channelName;
            record.channelDisplayName = channelDisplayName;
            record.userId = userId;
            record.platform = platform;
            record.roles = roles;
            record.displayPriority = displayPriority;
            this->chatters_.insert(key, std::move(record));
            return true;
        }

        bool changed = false;
        const auto combinedRoles = static_cast<std::uint8_t>(it->roles | roles);
        if (it->roles != combinedRoles)
        {
            it->roles = combinedRoles;
            changed = true;
        }
        if (displayPriority > it->displayPriority &&
            !displayName.trimmed().isEmpty())
        {
            it->displayName = displayName.trimmed();
            it->displayPriority = displayPriority;
            changed = true;
        }
        if (it->channelName.isEmpty() && !channelName.isEmpty())
        {
            it->channelName = channelName;
            changed = true;
        }
        if (it->channelDisplayName.isEmpty() && !channelDisplayName.isEmpty())
        {
            it->channelDisplayName = channelDisplayName;
            changed = true;
        }
        if (it->userId.isEmpty() && !userId.isEmpty())
        {
            it->userId = userId;
            changed = true;
        }
        return changed;
    }

    bool setQuery(QString query)
    {
        query = query.trimmed();
        if (query == this->query_)
        {
            return false;
        }
        this->query_ = std::move(query);
        return true;
    }

    void rebuild(bool loading)
    {
        std::array<std::vector<QString>, 6> groups;
        for (auto it = this->chatters_.cbegin(); it != this->chatters_.cend();
             ++it)
        {
            if (!this->query_.isEmpty() &&
                !it->login.contains(this->query_, Qt::CaseInsensitive) &&
                !it->displayName.contains(this->query_, Qt::CaseInsensitive))
            {
                continue;
            }
            groups.at(static_cast<size_t>(primaryRole(it->roles)))
                .push_back(it.key());
        }

        for (auto &group : groups)
        {
            std::sort(group.begin(), group.end(),
                      [this](const auto &left, const auto &right) {
                          const auto leftIt = this->chatters_.constFind(left);
                          const auto rightIt = this->chatters_.constFind(right);
                          const auto displayOrder = QString::compare(
                              leftIt->displayName, rightIt->displayName,
                              Qt::CaseInsensitive);
                          return displayOrder == 0 ? left < right
                                                   : displayOrder < 0;
                      });
        }

        this->beginResetModel();
        this->rows_.clear();
        this->rows_.reserve(this->chatters_.size() +
                            static_cast<qsizetype>(groups.size()));
        for (size_t index = 0; index < groups.size(); ++index)
        {
            const auto role = static_cast<ChatterRole>(index);
            const auto &group = groups.at(index);
            if (group.empty())
            {
                continue;
            }

            this->rows_.push_back({RowKind::Group,
                                   {},
                                   groupName(role),
                                   static_cast<int>(group.size())});
            for (const auto &key : group)
            {
                this->rows_.push_back({RowKind::User, key, {}, 0});
            }
        }

        if (this->rows_.isEmpty())
        {
            QString text;
            if (loading)
            {
                text = QStringLiteral("Loading chatters…");
            }
            else if (!this->query_.isEmpty())
            {
                text = QStringLiteral("No matching chatters");
            }
            else
            {
                text = QStringLiteral("No chatters found");
            }
            this->rows_.push_back({RowKind::Status, {}, text, 0});
        }
        this->endResetModel();
    }

    int total() const
    {
        return this->chatters_.size();
    }

    std::pair<int, int> moderatorAndVipCounts() const
    {
        int moderators = 0;
        int vips = 0;
        for (auto it = this->chatters_.cbegin(); it != this->chatters_.cend();
             ++it)
        {
            moderators += (it->roles & ModeratorRole) != 0;
            vips += (it->roles & VipRole) != 0;
        }
        return {moderators, vips};
    }

    QHash<QString, int> countsByChannel(MessagePlatform platform) const
    {
        QHash<QString, int> counts;
        for (const auto &record : this->chatters_)
        {
            if (record.platform == platform)
            {
                ++counts[channelKey(platform, record.channelName)];
            }
        }
        return counts;
    }

    QString keyAt(const QModelIndex &index) const
    {
        if (!index.isValid() || index.row() < 0 ||
            index.row() >= this->rows_.size() ||
            this->rows_.at(index.row()).kind != RowKind::User)
        {
            return {};
        }
        return this->rows_.at(index.row()).key;
    }

    QModelIndex indexForKey(const QString &key) const
    {
        if (key.isEmpty())
        {
            return {};
        }
        for (int row = 0; row < this->rows_.size(); ++row)
        {
            if (this->rows_.at(row).kind == RowKind::User &&
                this->rows_.at(row).key == key)
            {
                return this->index(row, 0);
            }
        }
        return {};
    }

    QString anchorAt(const QModelIndex &index) const
    {
        if (!index.isValid() || index.row() < 0 ||
            index.row() >= this->rows_.size())
        {
            return {};
        }
        const auto &row = this->rows_.at(index.row());
        if (row.kind == RowKind::User)
        {
            return QStringLiteral("u:") + row.key;
        }
        if (row.kind == RowKind::Group)
        {
            return QStringLiteral("g:") + row.text;
        }
        return QStringLiteral("s:") + row.text;
    }

    QModelIndex indexForAnchor(const QString &anchor) const
    {
        if (anchor.size() < 3 || anchor.at(1) != QLatin1Char(':'))
        {
            return {};
        }
        const auto value = anchor.sliced(2);
        for (int row = 0; row < this->rows_.size(); ++row)
        {
            const auto &candidate = this->rows_.at(row);
            const bool matches =
                (anchor.startsWith(QStringLiteral("u:")) &&
                 candidate.kind == RowKind::User && candidate.key == value) ||
                (anchor.startsWith(QStringLiteral("g:")) &&
                 candidate.kind == RowKind::Group && candidate.text == value) ||
                (anchor.startsWith(QStringLiteral("s:")) &&
                 candidate.kind == RowKind::Status && candidate.text == value);
            if (matches)
            {
                return this->index(row, 0);
            }
        }
        return {};
    }

private:
    QHash<QString, ChatterRecord> chatters_;
    QVector<ChatterRow> rows_;
    QString query_;
};

class ChatterListDelegate final : public QStyledItemDelegate
{
public:
    explicit ChatterListDelegate(QObject *parent)
        : QStyledItemDelegate(parent)
    {
    }

    void setAppearance(const QColor &background, const QColor &hover,
                       const QColor &selected, const QColor &text,
                       const QColor &muted, const QColor &accent,
                       const QFont &font, const QFont &boldFont, float scale,
                       bool showPlatforms, bool showChannels)
    {
        this->background_ = background;
        this->hover_ = hover;
        this->selected_ = selected;
        this->text_ = text;
        this->muted_ = muted;
        this->accent_ = accent;
        this->font_ = font;
        this->boldFont_ = boldFont;
        this->scale_ = scale;
        this->showPlatforms_ = showPlatforms;
        this->showChannels_ = showChannels;
    }

    QSize sizeHint(const QStyleOptionViewItem &,
                   const QModelIndex &) const override
    {
        return {0, std::max(22, qRound(28 * this->scale_))};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        const auto kind = static_cast<RowKind>(index.data(RowKindRole).toInt());
        auto background = this->background_;
        if (kind == RowKind::User && option.state & QStyle::State_Selected)
        {
            background = this->selected_;
        }
        else if (kind == RowKind::User &&
                 option.state & QStyle::State_MouseOver)
        {
            background = this->hover_;
        }
        painter->fillRect(option.rect, background);

        const auto padding = qRound(8 * this->scale_);
        const auto content = option.rect.adjusted(padding, 0, -padding, 0);
        if (kind == RowKind::Group)
        {
            painter->setFont(this->boldFont_);
            painter->setPen(this->accent_);
            painter->drawText(content, Qt::AlignLeft | Qt::AlignVCenter,
                              index.data(Qt::DisplayRole).toString());

            painter->setFont(this->font_);
            painter->setPen(this->muted_);
            painter->drawText(
                content, Qt::AlignRight | Qt::AlignVCenter,
                localizeNumbers(index.data(GroupCountRole).toInt()));
        }
        else if (kind == RowKind::Status)
        {
            painter->setFont(this->font_);
            painter->setPen(this->muted_);
            painter->drawText(content, Qt::AlignCenter,
                              index.data(Qt::DisplayRole).toString());
        }
        else
        {
            const auto display = index.data(DisplayNameRole).toString();
            const auto login = index.data(LoginRole).toString();
            painter->setFont(this->font_);
            painter->setPen(this->text_);

            const QFontMetrics metrics(this->font_);
            const auto displayWidth = metrics.horizontalAdvance(display);
            const auto showLogin =
                display.compare(login, Qt::CaseInsensitive) != 0;
            QStringList secondaryParts;
            if (showLogin)
            {
                secondaryParts.push_back(QStringLiteral("@%1").arg(login));
            }
            if (this->showPlatforms_)
            {
                secondaryParts.push_back(
                    index.data(PlatformNameRole).toString());
            }
            if (this->showChannels_)
            {
                const auto channel =
                    index.data(ChannelDisplayNameRole).toString();
                if (!channel.isEmpty())
                {
                    secondaryParts.push_back(
                        QStringLiteral("#%1").arg(channel));
                }
            }
            const auto secondary = secondaryParts.join(QStringLiteral(" · "));
            const auto showSecondary = !secondary.isEmpty();
            const auto available = content.width();
            const auto titleWidth =
                showSecondary
                    ? std::min(displayWidth, std::max(80, available * 2 / 3))
                    : available;
            painter->drawText(
                QRect(content.left(), content.top(), titleWidth,
                      content.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                metrics.elidedText(display, Qt::ElideRight, titleWidth));

            if (showSecondary && titleWidth < available)
            {
                const auto left =
                    content.left() + titleWidth + qRound(8 * this->scale_);
                const auto width = std::max(0, content.right() - left + 1);
                painter->setPen(this->muted_);
                painter->drawText(
                    QRect(left, content.top(), width, content.height()),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    metrics.elidedText(secondary, Qt::ElideRight, width));
            }
        }
        painter->restore();
    }

private:
    QColor background_;
    QColor hover_;
    QColor selected_;
    QColor text_;
    QColor muted_;
    QColor accent_;
    QFont font_;
    QFont boldFont_;
    float scale_ = 1.F;
    bool showPlatforms_ = false;
    bool showChannels_ = false;
};

QString sourceStateText(SourceState state)
{
    switch (state)
    {
        case SourceState::Pending:
            return QStringLiteral("updating");
        case SourceState::Ready:
            return QStringLiteral("ready");
        case SourceState::Cached:
            return QStringLiteral("ready from cache");
        case SourceState::Partial:
            return QStringLiteral("some details unavailable");
        case SourceState::Failed:
            return QStringLiteral("unavailable");
        case SourceState::Skipped:
            return QStringLiteral("not used");
    }
    return {};
}

bool sourceIsReady(SourceState state)
{
    return state == SourceState::Ready || state == SourceState::Cached ||
           state == SourceState::Partial;
}

QString countLabel(int count, const QString &singular, const QString &plural)
{
    return QStringLiteral("%1 %2").arg(localizeNumbers(count),
                                       count == 1 ? singular : plural);
}

}  // namespace

class ChatterListWidgetPrivate
{
public:
    ChatterListWidgetPrivate(ChatterListWidget *owner,
                             std::shared_ptr<Channel> channel)
        : q(owner)
        , channel(std::move(channel))
        , model(new ChatterListModel(owner))
        , delegate(new ChatterListDelegate(owner))
    {
        if (auto multi = std::dynamic_pointer_cast<MultiChannel>(this->channel))
        {
            QSet<int> platforms;
            for (const auto &child : multi->channels())
            {
                if (!child.channel ||
                    child.platform == MultiChannel::Platform::YouTube ||
                    child.channel->isYouTubeChannel())
                {
                    continue;
                }
                this->localChannels.push_back(child.channel);
                const auto platform = child.channel->messagePlatform();
                platforms.insert(static_cast<int>(platform));
                this->channelDisplayNames.insert(
                    channelKey(platform, child.channel->getName()),
                    child.channel->getDisplayName());
                if (auto twitch =
                        std::dynamic_pointer_cast<TwitchChannel>(child.channel))
                {
                    this->twitchChannels.push_back(std::move(twitch));
                }
            }
            this->showPlatforms = platforms.size() > 1;
            this->showChannels = this->localChannels.size() > 1;
        }
        else
        {
            this->localChannels.push_back(this->channel);
            this->channelDisplayNames.insert(
                channelKey(this->channel->messagePlatform(),
                           this->channel->getName()),
                this->channel->getDisplayName());
            if (auto twitch =
                    std::dynamic_pointer_cast<TwitchChannel>(this->channel))
            {
                this->twitchChannels.push_back(std::move(twitch));
            }
        }
    }

    void initialize()
    {
        auto *layout = new QVBoxLayout();
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(6);

        auto *tools = new QHBoxLayout();
        tools->setContentsMargins(0, 0, 0, 0);
        tools->setSpacing(6);
        this->search = new QLineEdit(this->q);
        this->search->setPlaceholderText("Search User...");
        this->search->setClearButtonEnabled(true);
        this->refresh = new QPushButton(QStringLiteral("Refresh"), this->q);
        this->refresh->setToolTip(
            QStringLiteral("Refresh all available sources"));
        tools->addWidget(this->search, 1);
        tools->addWidget(this->refresh);

        auto *summary = new QVBoxLayout();
        summary->setContentsMargins(0, 0, 0, 0);
        summary->setSpacing(1);
        this->counts = new QLabel(this->q);
        this->sources = new QLabel(this->q);
        this->sources->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        summary->addWidget(this->counts);
        summary->addWidget(this->sources);

        this->list = new QListView(this->q);
        this->list->setModel(this->model);
        this->list->setItemDelegate(this->delegate);
        this->list->setEditTriggers(QAbstractItemView::NoEditTriggers);
        this->list->setMouseTracking(true);
        this->list->setSelectionMode(QAbstractItemView::SingleSelection);
        this->list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        this->list->setUniformItemSizes(true);

        layout->addLayout(tools);
        layout->addLayout(summary);
        layout->addWidget(this->list, 1);
        this->q->getLayoutContainer()->setLayout(layout);

        this->searchUpdate.setSingleShot(true);
        this->searchUpdate.setInterval(120);
        QObject::connect(this->search, &QLineEdit::textChanged, this->q,
                         [this] {
                             this->searchUpdate.start();
                         });
        QObject::connect(&this->searchUpdate, &QTimer::timeout, this->q,
                         [this] {
                             if (this->model->setQuery(this->search->text()))
                             {
                                 this->refreshModelPreservingView();
                             }
                         });
        QObject::connect(this->refresh, &QPushButton::clicked, this->q, [this] {
            this->reload(true);
        });
        QObject::connect(
            this->list, &QListView::activated, this->q,
            [this](const QModelIndex &index) {
                const auto login = index.data(LoginRole).toString();
                if (!login.isEmpty())
                {
                    this->q->userClicked(login,
                                         static_cast<MessagePlatform>(
                                             index.data(PlatformRole).toInt()),
                                         index.data(ChannelNameRole).toString(),
                                         index.data(UserIdRole).toString());
                }
            });

        this->liveUpdate.setSingleShot(true);
        this->liveUpdate.setInterval(1200);
        QObject::connect(&this->liveUpdate, &QTimer::timeout, this->q, [this] {
            this->refreshModelPreservingView();
            this->updateSummary();
        });

        this->statusAnimation.setInterval(400);
        QObject::connect(&this->statusAnimation, &QTimer::timeout, this->q,
                         [this] {
                             if (this->pendingRequests() <= 0)
                             {
                                 this->statusAnimation.stop();
                                 return;
                             }
                             this->statusDots = this->statusDots % 3 + 1;
                             this->updateSourceStatus();
                         });

        const QPointer<ChatterListWidget> guard(this->q);
        this->messageConnection = this->channel->messageAppended.connect(
            [this, guard](MessagePtr &message, std::optional<MessageFlags>) {
                if (!guard || !message || !this->addMessage(*message))
                {
                    return;
                }
                if (!this->liveUpdate.isActive())
                {
                    this->liveUpdate.setInterval(this->liveUpdateInterval());
                    this->liveUpdate.start();
                }
            });

        HotkeyController::HotkeyMap actions{
            {"delete",
             [this](const std::vector<QString> &) -> QString {
                 this->q->close();
                 return {};
             }},
            {"accept", nullptr},
            {"reject", nullptr},
            {"scrollPage", nullptr},
            {"openTab", nullptr},
            {"search",
             [this](const std::vector<QString> &) -> QString {
                 this->search->setFocus();
                 this->search->selectAll();
                 return {};
             }},
        };
        getApp()->getHotkeys()->shortcutsForCategory(
            HotkeyCategory::PopupWindow, actions, this->q);

        this->reload(false);
        this->search->setFocus();
    }

    QString channelDisplayName(MessagePlatform platform,
                               const QString &channelName) const
    {
        const auto display = this->channelDisplayNames.constFind(
            channelKey(platform, channelName));
        return display == this->channelDisplayNames.cend() ? channelName
                                                           : display.value();
    }

    void refreshModelPreservingView()
    {
        const auto selectedKey = this->model->keyAt(this->list->currentIndex());
        const auto topIndex = this->list->indexAt(QPoint(1, 1));
        const auto topAnchor = this->model->anchorAt(topIndex);
        const auto topOffset =
            topIndex.isValid() ? this->list->visualRect(topIndex).top() : 0;

        this->liveUpdate.stop();
        this->model->rebuild(this->pendingRequests() > 0);

        const auto restoredTop = this->model->indexForAnchor(topAnchor);
        if (restoredTop.isValid())
        {
            this->list->scrollTo(restoredTop, QAbstractItemView::PositionAtTop);
            if (topOffset != 0)
            {
                auto *scrollbar = this->list->verticalScrollBar();
                scrollbar->setValue(scrollbar->value() - topOffset);
            }
        }

        const auto restoredSelection = this->model->indexForKey(selectedKey);
        if (restoredSelection.isValid())
        {
            this->list->selectionModel()->setCurrentIndex(
                restoredSelection, QItemSelectionModel::ClearAndSelect |
                                       QItemSelectionModel::Rows);
        }
    }

    int liveUpdateInterval() const
    {
        const auto total = this->model->total();
        if (total >= 10000)
        {
            return 5000;
        }
        if (total >= 2000)
        {
            return 2500;
        }
        return 1200;
    }

    bool addMessage(const Message &message)
    {
        if (message.loginName.isEmpty() ||
            message.platform == MessagePlatform::YouTube)
        {
            return false;
        }

        const auto roleMatchesChannel =
            message.platform == MessagePlatform::AnyOrTwitch &&
            (message.channelName.isEmpty() ||
             std::ranges::any_of(
                 this->twitchChannels, [&](const auto &channel) {
                     return message.channelName.compare(
                                channel->getName(), Qt::CaseInsensitive) == 0;
                 }));
        auto roles = rolesFromMessage(message, roleMatchesChannel);
        const auto userId = message.userID;
        if (!message.channelName.isEmpty() &&
            message.loginName.compare(message.channelName,
                                      Qt::CaseInsensitive) == 0)
        {
            roles |= BroadcasterRole;
        }
        return this->model->add(
            message.loginName, message.displayName, roles, 2, message.platform,
            message.channelName,
            this->channelDisplayName(message.platform, message.channelName),
            userId);
    }

    void addLocalData()
    {
        for (const auto &localChannel : this->localChannels)
        {
            const auto platform = localChannel->messagePlatform();
            if (platform == MessagePlatform::YouTube)
            {
                continue;
            }
            this->model->add(localChannel->getName(),
                             localChannel->getDisplayName(), BroadcasterRole, 2,
                             platform, localChannel->getName(),
                             localChannel->getDisplayName());

            if (auto *channelChatters =
                    dynamic_cast<ChannelChatters *>(localChannel.get()))
            {
                const auto chatters = channelChatters->accessChatters();
                for (const auto &[login, displayName] : chatters->all())
                {
                    this->model->add(login, displayName, NoRole, 1, platform,
                                     localChannel->getName(),
                                     localChannel->getDisplayName());
                }
            }
        }

        for (const auto &message :
             this->channel->getMessageSnapshot(CHATTER_LIMIT))
        {
            if (message)
            {
                this->addMessage(*message);
            }
        }
    }

    void reload(bool forceCommunity)
    {
        if (this->pendingRequests() > 0)
        {
            return;
        }

        ++this->generation;
        const auto currentGeneration = this->generation;
        this->twitchPending = 0;
        this->twitchSuccesses = 0;
        this->twitchFailures = 0;
        this->twitchTruncated = false;
        this->twitchQueue.clear();
        this->twitchActive = 0;
        this->communityPending = 0;
        this->communitySuccesses = 0;
        this->communityFailures = 0;
        this->communityCached = 0;
        this->communityQueue.clear();
        this->communityActive = 0;
        this->communityDeferred = false;
        this->communityLimitedByCapacity = false;
        this->twitchState = SourceState::Skipped;
        this->communityState = SourceState::Skipped;
        this->model->clear();
        this->addLocalData();

        auto mode = getSettings()->chatterListDataMode.getValue();
        if (mode != QStringLiteral("best") &&
            mode != QStringLiteral("twitch") &&
            mode != QStringLiteral("community") &&
            mode != QStringLiteral("local"))
        {
            mode = QStringLiteral("best");
        }
        if (mode == QStringLiteral("best") || mode == QStringLiteral("twitch"))
        {
            this->requestTwitch(currentGeneration);
        }
        if (mode == QStringLiteral("best") ||
            mode == QStringLiteral("community"))
        {
            if (this->twitchPending > 0)
            {
                this->communityDeferred = true;
                this->communityForce = forceCommunity;
            }
            else
            {
                this->requestCommunity(currentGeneration, forceCommunity);
            }
        }

        this->refreshModelPreservingView();
        this->updateSummary();
    }

    void beginTwitchRequest()
    {
        ++this->twitchPending;
        this->twitchState = SourceState::Pending;
    }

    void queueTwitchRequest(std::function<void()> request)
    {
        this->beginTwitchRequest();
        this->twitchQueue.push_back(std::move(request));
    }

    void pumpTwitchRequests(int requestGeneration)
    {
        if (requestGeneration != this->generation)
        {
            return;
        }

        while (this->twitchActive < TWITCH_MAX_CONCURRENT_REQUESTS &&
               !this->twitchQueue.empty())
        {
            auto request = std::move(this->twitchQueue.front());
            this->twitchQueue.pop_front();
            ++this->twitchActive;
            request();
        }
    }

    void finishTwitchRequest(int requestGeneration, bool success)
    {
        if (requestGeneration != this->generation)
        {
            return;
        }
        this->twitchPending = std::max(0, this->twitchPending - 1);
        this->twitchActive = std::max(0, this->twitchActive - 1);
        if (success)
        {
            ++this->twitchSuccesses;
        }
        else
        {
            ++this->twitchFailures;
        }

        if (this->twitchPending == 0)
        {
            if (this->twitchSuccesses == 0)
            {
                this->twitchState = SourceState::Failed;
            }
            else if (this->twitchFailures > 0 || this->twitchTruncated)
            {
                this->twitchState = SourceState::Partial;
            }
            else
            {
                this->twitchState = SourceState::Ready;
            }
            if (this->communityDeferred)
            {
                this->communityDeferred = false;
                this->requestCommunity(requestGeneration, this->communityForce);
            }
            this->refreshModelPreservingView();
        }
        this->updateSummary();
        this->pumpTwitchRequests(requestGeneration);
    }

    void requestTwitch(int requestGeneration)
    {
        const auto account = getApp()->getAccounts()->twitch.getCurrent();
        if (!account || account->isAnon() || account->getUserId().isEmpty())
        {
            return;
        }

        const auto eligibleChannels = std::ranges::count_if(
            this->twitchChannels, [](const auto &channel) {
                return channel->hasModRights() && !channel->roomId().isEmpty();
            });
        const auto available =
            std::max(0, CHATTER_LIMIT - this->model->total());
        const auto baseChatterLimit =
            eligibleChannels == 0
                ? 0
                : std::min(TWITCH_CHATTER_LIMIT,
                           available / static_cast<int>(eligibleChannels));
        auto chatterRemainder =
            eligibleChannels == 0
                ? 0
                : available % static_cast<int>(eligibleChannels);
        const QPointer<ChatterListWidget> guard(this->q);
        for (const auto &channel : this->twitchChannels)
        {
            if (!channel->hasModRights() || channel->roomId().isEmpty())
            {
                continue;
            }

            const auto channelName = channel->getName();
            const auto roomId = channel->roomId();
            const auto accountId = account->getUserId();
            const auto chatterLimit =
                baseChatterLimit + (chatterRemainder > 0 ? 1 : 0);
            chatterRemainder = std::max(0, chatterRemainder - 1);
            if (chatterLimit > 0)
            {
                this->queueTwitchRequest([this, guard, requestGeneration,
                                          channelName, roomId, accountId,
                                          chatterLimit] {
                    getHelix()->getChatters(
                        roomId, accountId, chatterLimit, this->q,
                        [this, guard, requestGeneration, channelName,
                         chatterLimit](const HelixChatters &chatters) {
                            if (!guard || requestGeneration != this->generation)
                            {
                                return;
                            }
                            const auto targetTotal =
                                std::min(CHATTER_LIMIT,
                                         this->model->total() + chatterLimit);
                            this->model->reserve(targetTotal);
                            for (const auto &login : chatters.chatters)
                            {
                                if (this->model->total() >= targetTotal)
                                {
                                    break;
                                }
                                this->model->add(
                                    login, login, NoRole, 1,
                                    MessagePlatform::AnyOrTwitch, channelName,
                                    this->channelDisplayName(
                                        MessagePlatform::AnyOrTwitch,
                                        channelName));
                            }
                            if (chatters.total > chatterLimit)
                            {
                                this->twitchTruncated = true;
                            }
                            this->finishTwitchRequest(requestGeneration, true);
                        },
                        [this, guard, requestGeneration](HelixGetChattersError,
                                                         const QString &) {
                            if (guard)
                            {
                                this->finishTwitchRequest(requestGeneration,
                                                          false);
                            }
                        });
                });
            }

            if (!channel->isBroadcaster())
            {
                continue;
            }

            this->queueTwitchRequest([this, guard, requestGeneration,
                                      channelName, roomId] {
                getHelix()->getModerators(
                    roomId, 1000, this->q,
                    [this, guard, requestGeneration,
                     channelName](const auto &moderators) {
                        if (!guard || requestGeneration != this->generation)
                        {
                            return;
                        }
                        for (const auto &moderator : moderators)
                        {
                            this->model->add(
                                moderator.userLogin, moderator.userName,
                                ModeratorRole, 3, MessagePlatform::AnyOrTwitch,
                                channelName,
                                this->channelDisplayName(
                                    MessagePlatform::AnyOrTwitch, channelName));
                        }
                        this->finishTwitchRequest(requestGeneration, true);
                    },
                    [this, guard, requestGeneration](HelixGetModeratorsError,
                                                     const QString &) {
                        if (guard)
                        {
                            this->finishTwitchRequest(requestGeneration, false);
                        }
                    });
            });

            this->queueTwitchRequest([this, guard, requestGeneration,
                                      channelName, roomId] {
                getHelix()->getChannelVIPs(
                    roomId, this->q,
                    [this, guard, requestGeneration,
                     channelName](const auto &vips) {
                        if (!guard || requestGeneration != this->generation)
                        {
                            return;
                        }
                        for (const auto &vip : vips)
                        {
                            this->model->add(
                                vip.userLogin, vip.userName, VipRole, 3,
                                MessagePlatform::AnyOrTwitch, channelName,
                                this->channelDisplayName(
                                    MessagePlatform::AnyOrTwitch, channelName));
                        }
                        this->finishTwitchRequest(requestGeneration, true);
                    },
                    [this, guard, requestGeneration](HelixListVIPsError,
                                                     const QString &) {
                        if (guard)
                        {
                            this->finishTwitchRequest(requestGeneration, false);
                        }
                    });
            });
        }
        this->pumpTwitchRequests(requestGeneration);
    }

    void pumpCommunityRequests(int requestGeneration, bool force)
    {
        const QPointer<ChatterListWidget> guard(this->q);
        while (this->communityActive == 0 && !this->communityQueue.empty())
        {
            auto item = std::move(this->communityQueue.front());
            this->communityQueue.pop_front();
            ++this->communityActive;

            const auto channelName = item.channelName;
            requestCommunityChatters(
                channelName, force,
                [this, guard, requestGeneration, force, item = std::move(item)](
                    std::shared_ptr<const CommunityPayload> payload,
                    bool fromCache) {
                    if (!guard || requestGeneration != this->generation)
                    {
                        return;
                    }

                    this->communityActive =
                        std::max(0, this->communityActive - 1);
                    this->communityPending =
                        std::max(0, this->communityPending - 1);
                    if (!payload)
                    {
                        ++this->communityFailures;
                    }
                    else
                    {
                        ++this->communitySuccesses;
                        this->communityCached += fromCache;
                        if (payload->reportedCount >
                            static_cast<int>(payload->users.size()))
                        {
                            this->communityLimitedByCapacity = true;
                        }
                        const auto targetTotal =
                            std::min(CHATTER_LIMIT,
                                     this->model->total() + item.mergeLimit);
                        this->model->reserve(targetTotal);
                        for (const auto &user : payload->users)
                        {
                            if (user.roles == NoRole &&
                                this->model->total() >= targetTotal)
                            {
                                this->communityLimitedByCapacity = true;
                                break;
                            }
                            this->model->add(user.login, user.login, user.roles,
                                             1, MessagePlatform::AnyOrTwitch,
                                             item.channelName,
                                             item.channelDisplayName);
                        }
                    }

                    if (this->communityPending == 0)
                    {
                        if (this->communitySuccesses == 0)
                        {
                            this->communityState = SourceState::Failed;
                        }
                        else if (this->communityFailures > 0 ||
                                 this->communityLimitedByCapacity)
                        {
                            this->communityState = SourceState::Partial;
                        }
                        else if (this->communityCached ==
                                 this->communitySuccesses)
                        {
                            this->communityState = SourceState::Cached;
                        }
                        else
                        {
                            this->communityState = SourceState::Ready;
                        }
                        this->refreshModelPreservingView();
                    }
                    this->updateSummary();
                    this->pumpCommunityRequests(requestGeneration, force);
                });
        }
    }

    void requestCommunity(int requestGeneration, bool force)
    {
        if (this->twitchChannels.empty())
        {
            return;
        }

        const auto available =
            std::max(0, CHATTER_LIMIT - this->model->total());
        this->communityQueue.clear();
        this->communityActive = 0;
        this->communityLimitedByCapacity =
            available < static_cast<int>(this->twitchChannels.size());
        const auto observedByChannel =
            this->model->countsByChannel(MessagePlatform::AnyOrTwitch);
        std::vector<int> channelCaps;
        channelCaps.reserve(this->twitchChannels.size());
        for (const auto &channel : this->twitchChannels)
        {
            auto cap = CHATTER_LIMIT;
            const auto stream = channel->accessStreamStatus();
            if (stream->live && stream->viewerCount > 0)
            {
                cap = static_cast<int>(std::min<std::uint64_t>(
                    stream->viewerCount, CHATTER_LIMIT));
            }
            const auto observed = observedByChannel.value(
                channelKey(MessagePlatform::AnyOrTwitch, channel->getName()));
            channelCaps.push_back(std::max(0, cap - observed));
        }

        std::vector<int> channelLimits(this->twitchChannels.size(), 0);
        auto remaining = available;
        while (remaining > 0)
        {
            int activeChannels = 0;
            for (size_t index = 0; index < channelLimits.size(); ++index)
            {
                activeChannels +=
                    channelLimits.at(index) < channelCaps.at(index);
            }
            if (activeChannels == 0)
            {
                break;
            }

            const auto share =
                std::max(1, (remaining + activeChannels - 1) / activeChannels);
            for (size_t index = 0;
                 index < channelLimits.size() && remaining > 0; ++index)
            {
                const auto room =
                    channelCaps.at(index) - channelLimits.at(index);
                if (room <= 0)
                {
                    continue;
                }
                const auto added = std::min({room, share, remaining});
                channelLimits.at(index) += added;
                remaining -= added;
            }
        }
        for (size_t index = 0; index < this->twitchChannels.size(); ++index)
        {
            const auto &channel = this->twitchChannels.at(index);
            const auto limit = channelLimits.at(index);
            this->communityQueue.push_back(
                {channel->getName(), channel->getDisplayName(), limit});
        }

        this->communityPending = static_cast<int>(this->communityQueue.size());
        this->communityState = SourceState::Pending;
        this->pumpCommunityRequests(requestGeneration, force);
    }

    void updateSummary()
    {
        const auto [moderators, vips] = this->model->moderatorAndVipCounts();
        this->counts->setText(
            QStringLiteral("%1 · %2 · %3")
                .arg(countLabel(this->model->total(), QStringLiteral("chatter"),
                                QStringLiteral("chatters")),
                     countLabel(moderators, QStringLiteral("moderator"),
                                QStringLiteral("moderators")),
                     countLabel(vips, QStringLiteral("VIP"),
                                QStringLiteral("VIPs"))));

        this->updateSourceStatus();
    }

    void updateSourceStatus()
    {
        int sourceCount = 1;
        sourceCount += sourceIsReady(this->twitchState);
        sourceCount += sourceIsReady(this->communityState);
        const auto partial = this->twitchState == SourceState::Partial ||
                             this->twitchState == SourceState::Failed ||
                             this->communityState == SourceState::Partial ||
                             this->communityState == SourceState::Failed;
        auto status = QStringLiteral("Updated now");
        if (this->pendingRequests() > 0)
        {
            if (!this->statusAnimation.isActive())
            {
                this->statusDots = 1;
                this->statusAnimation.start();
            }
            status = QStringLiteral("Updating%1")
                         .arg(QString(this->statusDots, QLatin1Char('.')));
        }
        else
        {
            this->statusAnimation.stop();
            this->statusDots = 0;
            if (partial)
            {
                status = QStringLiteral("Partial list");
            }
            else if (this->communityState == SourceState::Cached)
            {
                status = QStringLiteral("Updated recently");
            }
        }
        this->sources->setText(QStringLiteral("%1 %2 · %3")
                                   .arg(sourceCount)
                                   .arg(sourceCount == 1
                                            ? QStringLiteral("source")
                                            : QStringLiteral("sources"))
                                   .arg(status));
        this->sources->setToolTip(
            QStringLiteral("Chat session: %1\nTwitch: %2\nTackling: %3")
                .arg(QStringLiteral("ready"),
                     sourceStateText(this->twitchState),
                     sourceStateText(this->communityState)));
        this->refresh->setEnabled(this->pendingRequests() == 0);
    }

    int pendingRequests() const
    {
        return this->twitchPending + this->communityPending;
    }

    void applyTheme(float scale)
    {
        const auto *theme = getTheme();
        auto palette = theme->palette;
        palette.setColor(QPalette::Window, theme->window.background);
        palette.setColor(QPalette::WindowText, theme->window.text);
        palette.setColor(QPalette::Base,
                         theme->tabs.regular.backgrounds.regular);
        palette.setColor(QPalette::AlternateBase,
                         theme->tabs.regular.backgrounds.hover);
        palette.setColor(QPalette::Text, theme->window.text);
        palette.setColor(QPalette::Button,
                         theme->tabs.selected.backgrounds.regular);
        palette.setColor(QPalette::ButtonText, theme->window.text);
        palette.setColor(QPalette::Highlight,
                         theme->tabs.selected.backgrounds.regular);
        palette.setColor(QPalette::HighlightedText, theme->window.text);
        palette.setColor(QPalette::PlaceholderText,
                         theme->messages.textColors.chatPlaceholder);
        palette.setColor(QPalette::Link, theme->accent);
        palette.setColor(QPalette::Mid, theme->tabs.dividerLine);
        palette.setColor(QPalette::ToolTipBase,
                         theme->tabs.selected.backgrounds.regular);
        palette.setColor(QPalette::ToolTipText, theme->window.text);
        palette.setColor(QPalette::Disabled, QPalette::Text,
                         theme->tabs.regular.text);
        palette.setColor(QPalette::Disabled, QPalette::ButtonText,
                         theme->tabs.regular.text);
        palette.setColor(QPalette::Disabled, QPalette::Button,
                         theme->tabs.regular.backgrounds.unfocused);
        this->q->setPalette(palette);
        this->list->setPalette(palette);
        this->refresh->setPalette(palette);

        auto inputPalette = palette;
        inputPalette.setColor(QPalette::Base, theme->splits.input.background);
        inputPalette.setColor(QPalette::Text, theme->splits.input.text);
        this->search->setPalette(inputPalette);

        auto mutedPalette = palette;
        mutedPalette.setColor(QPalette::WindowText, theme->tabs.regular.text);
        this->sources->setPalette(mutedPalette);

        const auto font =
            getApp()->getFonts()->getFont(FontStyle::UiMedium, scale);
        const auto boldFont =
            getApp()->getFonts()->getFont(FontStyle::UiMediumBold, scale);
        this->q->setFont(font);
        this->counts->setFont(boldFont);
        this->delegate->setAppearance(
            theme->tabs.regular.backgrounds.regular,
            theme->tabs.regular.backgrounds.hover,
            theme->tabs.selected.backgrounds.regular, theme->window.text,
            theme->tabs.regular.text, theme->accent, font, boldFont, scale,
            this->showPlatforms, this->showChannels);
        this->list->doItemsLayout();
        this->list->viewport()->update();
    }

    ChatterListWidget *q;
    std::shared_ptr<Channel> channel;
    std::vector<ChannelPtr> localChannels;
    std::vector<std::shared_ptr<TwitchChannel>> twitchChannels;
    QHash<QString, QString> channelDisplayNames;
    bool showPlatforms = false;
    bool showChannels = false;
    ChatterListModel *model;
    ChatterListDelegate *delegate;
    QLineEdit *search = nullptr;
    QPushButton *refresh = nullptr;
    QLabel *counts = nullptr;
    QLabel *sources = nullptr;
    QListView *list = nullptr;
    QTimer liveUpdate;
    QTimer searchUpdate;
    QTimer statusAnimation;
    pajlada::Signals::ScopedConnection messageConnection;
    SourceState twitchState = SourceState::Skipped;
    SourceState communityState = SourceState::Skipped;
    int generation = 0;
    int twitchPending = 0;
    int twitchSuccesses = 0;
    int twitchFailures = 0;
    bool twitchTruncated = false;
    std::deque<std::function<void()>> twitchQueue;
    int twitchActive = 0;
    int communityPending = 0;
    int communitySuccesses = 0;
    int communityFailures = 0;
    int communityCached = 0;
    std::deque<CommunityWorkItem> communityQueue;
    int communityActive = 0;
    bool communityDeferred = false;
    bool communityForce = false;
    bool communityLimitedByCapacity = false;
    int statusDots = 0;
};

ChatterListWidget::ChatterListWidget(std::shared_ptr<Channel> channel,
                                     QWidget *parent)
    : BaseWindow({}, parent)
    , d_(std::make_unique<ChatterListWidgetPrivate>(this, std::move(channel)))
{
    assert(this->d_->channel != nullptr);
    assert(!this->d_->localChannels.empty());
    this->setWindowTitle("Chatter List - " + this->d_->channel->getName());
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setMinimumSize(360, 360);
    this->d_->initialize();
    this->themeChangedEvent();
}

ChatterListWidget::~ChatterListWidget() = default;

bool ChatterListWidget::supportsChannel(const Channel *channel)
{
    const auto supported = [](const Channel *candidate) {
        return candidate && candidate->getType() == Channel::Type::Twitch;
    };
    if (const auto *multi = dynamic_cast<const MultiChannel *>(channel))
    {
        return std::ranges::any_of(multi->channels(), [&](const auto &child) {
            return supported(child.channel.get());
        });
    }
    return supported(channel);
}

const QString &ChatterListWidget::channelName() const
{
    return this->d_->channel->getName();
}

void ChatterListWidget::themeChangedEvent()
{
    BaseWindow::themeChangedEvent();
    if (this->d_ && this->d_->list)
    {
        this->d_->applyTheme(this->scale());
    }
}

void ChatterListWidget::scaleChangedEvent(float scale)
{
    BaseWindow::scaleChangedEvent(scale);
    this->setMinimumSize(qRound(360 * scale), qRound(360 * scale));
    if (this->d_ && this->d_->list)
    {
        this->d_->applyTheme(scale);
    }
}

}  // namespace chatterino

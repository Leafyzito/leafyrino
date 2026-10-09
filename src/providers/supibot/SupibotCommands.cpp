#include "providers/supibot/SupibotCommands.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "singletons/Paths.hpp"
#include "util/QStringHash.hpp"  // IWYU pragma: keep

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <array>
#include <unordered_set>

namespace chatterino {

namespace {

constexpr auto COMMANDS_URL = "https://supinic.com/api/bot/command/list/";
constexpr auto CHANNELS_URL =
    "https://supinic.com/api/bot/channel/list?platformName=twitch";
constexpr qsizetype MAX_PAYLOAD_SIZE = 2 * 1024 * 1024;
constexpr qsizetype MAX_CHANNEL_PAYLOAD_SIZE = 8 * 1024 * 1024;
constexpr qsizetype MAX_METADATA_SIZE = 4096;
constexpr qsizetype MAX_COMMANDS = 2000;
constexpr qsizetype MAX_ALIASES = 5000;
constexpr qsizetype MAX_USER_ALIASES = 1000;
constexpr qsizetype MAX_CHANNELS = 20000;
constexpr qsizetype MAX_USAGE_LENGTH = 512;

QString cachePath(const QString &fileName)
{
    return getApp()->getPaths().cacheFilePath(fileName);
}

QString currentLogin()
{
    const auto user = getApp()->getAccounts()->twitch.getCurrent();
    if (!user || user->isAnon())
    {
        return {};
    }
    return user->getUserName().trimmed().toLower();
}

bool validCommandName(const QString &name)
{
    if (name.isEmpty() || name.size() > 64)
    {
        return false;
    }
    return std::ranges::none_of(name, [](QChar character) {
        return character.isSpace();
    });
}

QString cleanDescription(const QString &raw)
{
    auto text = raw.trimmed().left(MAX_USAGE_LENGTH);
    text.replace(QLatin1Char('\r'), QLatin1Char(' '));
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return text.simplified();
}

bool acceptablePayload(const QByteArray &payload, qsizetype maximum)
{
    return !payload.isEmpty() && payload.size() <= maximum;
}

QJsonArray dataArray(const QByteArray &payload, qsizetype maximum)
{
    if (!acceptablePayload(payload, maximum))
    {
        return {};
    }
    const auto object = QJsonDocument::fromJson(payload).object();
    if (object.isEmpty() || !object.value(QStringLiteral("data")).isArray())
    {
        return {};
    }
    const auto status = object.value(QStringLiteral("statusCode"));
    if (status.isDouble() && status.toInt() != 200)
    {
        return {};
    }
    return object.value(QStringLiteral("data")).toArray();
}

std::vector<SupibotCommand> parseCommands(const QByteArray &payload)
{
    const auto data = dataArray(payload, MAX_PAYLOAD_SIZE);
    if (data.isEmpty())
    {
        return {};
    }

    std::vector<SupibotCommand> commands;
    std::unordered_set<QString> names;
    qsizetype count = 0;
    qsizetype aliasCount = 0;
    for (const auto &value : data)
    {
        if (++count > MAX_COMMANDS)
        {
            break;
        }
        const auto object = value.toObject();
        const auto name =
            object.value(QStringLiteral("name")).toString().trimmed();
        if (!validCommandName(name) || !names.insert(name.toLower()).second)
        {
            continue;
        }
        commands.push_back({
            .name = name,
            .usage = cleanDescription(
                object.value(QStringLiteral("description")).toString()),
            .alias = false,
        });

        for (const auto &aliasValue :
             object.value(QStringLiteral("aliases")).toArray())
        {
            if (aliasCount >= MAX_ALIASES)
            {
                break;
            }
            const auto alias = aliasValue.toString().trimmed();
            if (!validCommandName(alias) ||
                !names.insert(alias.toLower()).second)
            {
                continue;
            }
            auto usage = cleanDescription(
                object.value(QStringLiteral("description")).toString());
            if (usage.isEmpty())
            {
                usage = QStringLiteral("Alias for $%1").arg(name);
            }
            commands.push_back({
                .name = alias,
                .usage = std::move(usage),
                .alias = true,
            });
            ++aliasCount;
        }
    }
    return commands;
}

QSet<QString> parseChannels(const QByteArray &payload, bool *ok)
{
    const auto data = dataArray(payload, MAX_CHANNEL_PAYLOAD_SIZE);
    if (data.isEmpty())
    {
        *ok = false;
        return {};
    }

    QSet<QString> active;
    qsizetype count = 0;
    for (const auto &value : data)
    {
        if (++count > MAX_CHANNELS)
        {
            break;
        }
        const auto object = value.toObject();
        const auto mode = object.value(QStringLiteral("mode")).toString();
        if (mode == QStringLiteral("Last seen") ||
            mode == QStringLiteral("Read"))
        {
            continue;
        }
        const auto platform =
            object.value(QStringLiteral("platformName")).toString();
        if (!platform.isEmpty() && platform.compare(QStringLiteral("twitch"),
                                                    Qt::CaseInsensitive) != 0)
        {
            continue;
        }
        const auto name =
            object.value(QStringLiteral("name")).toString().trimmed().toLower();
        if (!name.isEmpty())
        {
            active.insert(name);
        }
    }
    *ok = true;
    return active;
}

std::vector<SupibotCommand> parseAliases(const QByteArray &payload, bool *ok)
{
    const auto data = dataArray(payload, MAX_PAYLOAD_SIZE);
    if (payload.isEmpty() || payload.size() > MAX_PAYLOAD_SIZE ||
        QJsonDocument::fromJson(payload).object().isEmpty())
    {
        *ok = false;
        return {};
    }
    if (!QJsonDocument::fromJson(payload)
             .object()
             .value(QStringLiteral("data"))
             .isArray())
    {
        *ok = false;
        return {};
    }

    std::vector<SupibotCommand> aliases;
    std::unordered_set<QString> names;
    qsizetype count = 0;
    for (const auto &value : data)
    {
        if (++count > MAX_USER_ALIASES)
        {
            break;
        }
        const auto object = value.toObject();
        const auto name =
            object.value(QStringLiteral("name")).toString().trimmed();
        if (!validCommandName(name) || !names.insert(name.toLower()).second)
        {
            continue;
        }
        auto usage = cleanDescription(
            object.value(QStringLiteral("description")).toString());
        const auto invocation =
            object.value(QStringLiteral("invocation")).toString().trimmed();
        if (usage.isEmpty() && validCommandName(invocation))
        {
            usage = QStringLiteral("Alias for $%1").arg(invocation);
        }
        aliases.push_back({
            .name = name,
            .usage = std::move(usage),
            .alias = true,
        });
    }
    *ok = true;
    return aliases;
}

void sortCommands(std::vector<SupibotCommand> &commands)
{
    std::ranges::stable_sort(commands, [](const auto &left, const auto &right) {
        return left.name.compare(right.name, Qt::CaseInsensitive) < 0;
    });
}

}  // namespace
SupibotCommands::SupibotCommands() = default;

void SupibotCommands::ensureLoaded()
{
    this->watchUser();
    if (!this->commandsState_.cacheChecked)
    {
        this->loadCommandCache();
    }
    if (!this->channelsState_.cacheChecked)
    {
        this->loadChannelCache();
    }
    if (!this->aliasesState_.cacheChecked)
    {
        this->aliasLogin_ = currentLogin();
        if (this->aliasLogin_.isEmpty())
        {
            this->aliasesState_.cacheChecked = true;
            this->aliasesState_.ready = true;
            this->aliasesState_.fetchedThisSession = true;
        }
        else
        {
            this->loadAliasCache();
        }
    }
    this->maybeRequest(Catalog::Commands);
    this->maybeRequest(Catalog::Channels);
    if (!this->aliasLogin_.isEmpty())
    {
        this->maybeRequest(Catalog::Aliases);
    }
}

const std::vector<SupibotCommand> &SupibotCommands::commands() const
{
    return this->commands_;
}

bool SupibotCommands::isLoading() const
{
    return this->commandsState_.requestInFlight ||
           this->channelsState_.requestInFlight ||
           this->aliasesState_.requestInFlight;
}

bool SupibotCommands::channelListReady() const
{
    return this->channelsState_.ready;
}

bool SupibotCommands::isActive(const QString &channelLogin) const
{
    if (!this->channelsState_.ready)
    {
        return false;
    }
    return this->activeChannels_.contains(channelLogin.trimmed().toLower());
}

void SupibotCommands::watchUser()
{
    if (this->watchingUser_)
    {
        return;
    }
    this->watchingUser_ = true;
    this->userChanged_ =
        getApp()->getAccounts()->twitch.currentUserChanged.connect([this] {
            this->onUserChanged();
        });
}

void SupibotCommands::onUserChanged()
{
    const auto login = currentLogin();
    if (login == this->aliasLogin_)
    {
        return;
    }
    this->aliasLogin_ = login;
    this->userAliases_.clear();
    this->aliasesState_ = {};
    this->aliasesState_.cacheChecked = true;
    if (login.isEmpty())
    {
        this->aliasesState_.ready = true;
        this->aliasesState_.fetchedThisSession = true;
    }
    else
    {
        this->loadAliasCache();
        this->maybeRequest(Catalog::Aliases);
    }
    this->rebuild();
    this->commandsUpdated.invoke();
}

void SupibotCommands::loadCommandCache()
{
    this->commandsState_.cacheChecked = true;
    QFile metadata(cachePath(this->metadataFile(Catalog::Commands)));
    if (metadata.open(QIODevice::ReadOnly))
    {
        this->commandsState_.etag =
            QJsonDocument::fromJson(metadata.read(MAX_METADATA_SIZE + 1))
                .object()
                .value(QStringLiteral("etag"))
                .toString();
    }
    QFile cache(cachePath(this->cacheFile(Catalog::Commands)));
    if (!cache.open(QIODevice::ReadOnly))
    {
        return;
    }
    auto commands = parseCommands(cache.read(MAX_PAYLOAD_SIZE + 1));
    if (!commands.empty())
    {
        this->baseCommands_ = std::move(commands);
        this->commandsState_.ready = true;
        this->rebuild();
    }
}

void SupibotCommands::loadChannelCache()
{
    this->channelsState_.cacheChecked = true;
    QFile metadata(cachePath(this->metadataFile(Catalog::Channels)));
    if (metadata.open(QIODevice::ReadOnly))
    {
        this->channelsState_.etag =
            QJsonDocument::fromJson(metadata.read(MAX_METADATA_SIZE + 1))
                .object()
                .value(QStringLiteral("etag"))
                .toString();
    }
    QFile cache(cachePath(this->cacheFile(Catalog::Channels)));
    if (!cache.open(QIODevice::ReadOnly))
    {
        return;
    }
    bool ok = false;
    auto channels =
        parseChannels(cache.read(MAX_CHANNEL_PAYLOAD_SIZE + 1), &ok);
    if (ok)
    {
        this->activeChannels_ = std::move(channels);
        this->channelsState_.ready = true;
    }
}

void SupibotCommands::loadAliasCache()
{
    this->aliasesState_.cacheChecked = true;
    QString cachedLogin;
    QFile metadata(cachePath(this->metadataFile(Catalog::Aliases)));
    if (metadata.open(QIODevice::ReadOnly))
    {
        const auto object =
            QJsonDocument::fromJson(metadata.read(MAX_METADATA_SIZE + 1))
                .object();
        cachedLogin = object.value(QStringLiteral("login")).toString();
        if (cachedLogin.compare(this->aliasLogin_, Qt::CaseInsensitive) == 0)
        {
            this->aliasesState_.etag =
                object.value(QStringLiteral("etag")).toString();
        }
    }
    if (cachedLogin.compare(this->aliasLogin_, Qt::CaseInsensitive) != 0)
    {
        return;
    }
    QFile cache(cachePath(this->cacheFile(Catalog::Aliases)));
    if (!cache.open(QIODevice::ReadOnly))
    {
        return;
    }
    bool ok = false;
    auto aliases = parseAliases(cache.read(MAX_PAYLOAD_SIZE + 1), &ok);
    if (ok)
    {
        this->userAliases_ = std::move(aliases);
        this->aliasesState_.ready = true;
        this->rebuild();
    }
}

void SupibotCommands::maybeRequest(Catalog catalog)
{
    auto &fetch = this->state(catalog);
    if (fetch.fetchedThisSession || fetch.requestInFlight ||
        fetch.requestAttempts >= 3 ||
        (fetch.nextRetryAt.isValid() &&
         QDateTime::currentDateTimeUtc() < fetch.nextRetryAt))
    {
        return;
    }
    this->request(catalog, true);
}

void SupibotCommands::request(Catalog catalog, bool useEtag)
{
    auto &fetch = this->state(catalog);
    if (fetch.requestInFlight || fetch.requestAttempts >= 3)
    {
        return;
    }
    fetch.requestInFlight = true;
    ++fetch.requestAttempts;

    auto request = NetworkRequest(this->urlFor(catalog))
                       .header("Accept", "application/json")
                       .timeout(15000)
                       .caller(this);
    if (useEtag && !fetch.etag.isEmpty())
    {
        request = std::move(request).header("If-None-Match", fetch.etag);
    }
    const auto maximum = catalog == Catalog::Channels ? MAX_CHANNEL_PAYLOAD_SIZE
                                                      : MAX_PAYLOAD_SIZE;
    std::move(request)
        .onSuccess([this, catalog, maximum](const NetworkResult &result) {
            auto &fetch = this->state(catalog);
            if (result.status() == 304)
            {
                if (!fetch.ready)
                {
                    fetch.etag.clear();
                    fetch.retryWithoutEtag = true;
                    return;
                }
                fetch.fetchedThisSession = true;
                return;
            }
            const auto body = result.getData().left(maximum + 1);
            if (!this->applyPayload(catalog, body))
            {
                qCWarning(chatterinoApp)
                    << "[Supibot] Ignoring an invalid catalog response.";
                this->noteFailure(fetch);
                return;
            }
            fetch.etag = QString::fromUtf8(result.etag());
            fetch.fetchedThisSession = true;
            if (this->saveCache(catalog, result.getData()))
            {
                this->saveMetadata(catalog);
            }
        })
        .onError([this, catalog](const NetworkResult &result) {
            qCWarning(chatterinoApp)
                << "[Supibot] Catalog request failed:" << result.formatError();
            this->noteFailure(this->state(catalog));
        })
        .finally([this, catalog] {
            auto &fetch = this->state(catalog);
            fetch.requestInFlight = false;
            const auto retryWithoutEtag = fetch.retryWithoutEtag;
            fetch.retryWithoutEtag = false;
            this->commandsUpdated.invoke();
            if (retryWithoutEtag)
            {
                QTimer::singleShot(0, this, [this, catalog] {
                    this->request(catalog, false);
                });
            }
        })
        .execute();
}

bool SupibotCommands::applyPayload(Catalog catalog, const QByteArray &payload)
{
    switch (catalog)
    {
        case Catalog::Commands: {
            auto commands = parseCommands(payload);
            if (commands.empty())
            {
                return false;
            }
            this->baseCommands_ = std::move(commands);
            this->commandsState_.ready = true;
            this->rebuild();
            return true;
        }
        case Catalog::Channels: {
            bool ok = false;
            auto channels = parseChannels(payload, &ok);
            if (!ok)
            {
                return false;
            }
            this->activeChannels_ = std::move(channels);
            this->channelsState_.ready = true;
            return true;
        }
        case Catalog::Aliases: {
            bool ok = false;
            auto aliases = parseAliases(payload, &ok);
            if (!ok)
            {
                return false;
            }
            this->userAliases_ = std::move(aliases);
            this->aliasesState_.ready = true;
            this->rebuild();
            return true;
        }
    }
    return false;
}

void SupibotCommands::rebuild()
{
    std::vector<SupibotCommand> commands = this->baseCommands_;
    std::unordered_set<QString> names;
    for (const auto &command : commands)
    {
        names.insert(command.name.toLower());
    }
    for (const auto &alias : this->userAliases_)
    {
        if (names.insert(alias.name.toLower()).second)
        {
            commands.push_back(alias);
        }
    }
    sortCommands(commands);
    this->commands_ = std::move(commands);
}

void SupibotCommands::noteFailure(FetchState &state) const
{
    const auto delays = std::array{30, 120, 600};
    const auto delay =
        delays.at(std::min(state.requestAttempts - 1, int(delays.size()) - 1));
    state.nextRetryAt = QDateTime::currentDateTimeUtc().addSecs(delay);
}

bool SupibotCommands::saveCache(Catalog catalog,
                                const QByteArray &payload) const
{
    QSaveFile file(cachePath(this->cacheFile(catalog)));
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(payload) != payload.size())
    {
        return false;
    }
    return file.commit();
}

void SupibotCommands::saveMetadata(Catalog catalog) const
{
    QSaveFile file(cachePath(this->metadataFile(catalog)));
    if (!file.open(QIODevice::WriteOnly))
    {
        return;
    }
    QJsonObject object{{QStringLiteral("etag"), this->state(catalog).etag}};
    if (catalog == Catalog::Aliases)
    {
        object.insert(QStringLiteral("login"), this->aliasLogin_);
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    file.commit();
}

SupibotCommands::FetchState &SupibotCommands::state(Catalog catalog)
{
    switch (catalog)
    {
        case Catalog::Commands:
            return this->commandsState_;
        case Catalog::Channels:
            return this->channelsState_;
        case Catalog::Aliases:
            return this->aliasesState_;
    }
    return this->commandsState_;
}

const SupibotCommands::FetchState &SupibotCommands::state(Catalog catalog) const
{
    return const_cast<SupibotCommands *>(this)->state(catalog);
}

QString SupibotCommands::urlFor(Catalog catalog) const
{
    switch (catalog)
    {
        case Catalog::Commands:
            return QString::fromLatin1(COMMANDS_URL);
        case Catalog::Channels:
            return QString::fromLatin1(CHANNELS_URL);
        case Catalog::Aliases:
            return QStringLiteral(
                       "https://supinic.com/api/bot/user/%1/alias/list/")
                .arg(QString::fromUtf8(
                    QUrl::toPercentEncoding(this->aliasLogin_)));
    }
    return {};
}

QString SupibotCommands::cacheFile(Catalog catalog) const
{
    switch (catalog)
    {
        case Catalog::Commands:
            return QStringLiteral("supibot-commands.json");
        case Catalog::Channels:
            return QStringLiteral("supibot-channels.json");
        case Catalog::Aliases:
            return QStringLiteral("supibot-user-aliases.json");
    }
    return {};
}

QString SupibotCommands::metadataFile(Catalog catalog) const
{
    switch (catalog)
    {
        case Catalog::Commands:
            return QStringLiteral("supibot-commands.meta.json");
        case Catalog::Channels:
            return QStringLiteral("supibot-channels.meta.json");
        case Catalog::Aliases:
            return QStringLiteral("supibot-user-aliases.meta.json");
    }
    return {};
}

}  // namespace chatterino

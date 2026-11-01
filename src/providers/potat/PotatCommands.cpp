#include "providers/potat/PotatCommands.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "util/QStringHash.hpp"  // IWYU pragma: keep

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <array>
#include <unordered_set>

namespace chatterino {

namespace {

constexpr auto CATALOG_URL = "https://api.potat.app/help";
constexpr auto USERS_URL = "https://api.potat.app/users/";
constexpr auto CACHE_FILE = "potat-command-catalog.json";
constexpr auto META_FILE = "potat-command-catalog.meta.json";
constexpr qsizetype MAX_PAYLOAD_SIZE = 2 * 1024 * 1024;
constexpr qsizetype MAX_CHANNEL_PAYLOAD_SIZE = 1024 * 1024;
constexpr qsizetype MAX_METADATA_SIZE = 4096;
constexpr qsizetype MAX_CHANNEL_COMMANDS = 200;
constexpr qsizetype MAX_PREFIXES = 8;
constexpr qsizetype MAX_HELP_LENGTH = 512;
constexpr qsizetype MAX_COMMANDS = 1000;
constexpr qsizetype MAX_ALIASES = 3000;
constexpr qsizetype MAX_USAGE_LENGTH = 2048;

QString cachePath()
{
    return getApp()->getPaths().cacheFilePath(QString::fromLatin1(CACHE_FILE));
}

QString metadataPath()
{
    return getApp()->getPaths().cacheFilePath(QString::fromLatin1(META_FILE));
}

bool validCommandName(const QString &name)
{
    static const QRegularExpression expression(
        QStringLiteral("^[a-zA-Z0-9_-]{1,64}$"));
    return expression.match(name).hasMatch();
}

bool hasLeadingCommand(const QString &form, const QString &command)
{
    const auto bounded = form.trimmed().left(MAX_USAGE_LENGTH);
    if (!bounded.startsWith(QChar('#')))
    {
        return false;
    }
    qsizetype end = 1;
    while (end < bounded.size() && !bounded.at(end).isSpace())
    {
        ++end;
    }
    const auto names = bounded.mid(1, end - 1).split(QChar('/'));
    return std::ranges::all_of(names, validCommandName) &&
           names.contains(command, Qt::CaseInsensitive);
}

QString removeLeadingCommand(QString form, const QString &command)
{
    form = form.trimmed();
    if (hasLeadingCommand(form, command))
    {
        qsizetype end = 0;
        while (end < form.size() && !form.at(end).isSpace())
        {
            ++end;
        }
        form = form.mid(end).trimmed();
    }
    return form;
}

completion::CommandUsage normalizeUsage(const QString &raw,
                                        const QString &command)
{
    auto usage = raw.trimmed().left(MAX_USAGE_LENGTH);
    usage.replace(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                  QStringLiteral(" | "));
    auto forms = completion::splitCommandUsageForms(usage);
    bool alternatives = false;
    for (qsizetype i = 1; i < forms.size(); ++i)
    {
        alternatives |= hasLeadingCommand(forms.at(i), command);
    }
    bool hasOtherCommand = false;
    for (auto &form : forms)
    {
        form = removeLeadingCommand(std::move(form), command);
        for (const auto &field : completion::splitCommandUsageFields(form))
        {
            hasOtherCommand |= field.startsWith(QChar('#'));
        }
        if (form.isEmpty() && forms.size() > 1)
        {
            form = QStringLiteral("no arguments");
        }
    }
    return completion::compileCommandUsage(forms.join(QStringLiteral(" | ")),
                                           alternatives, hasOtherCommand);
}

QString flagValueHint(const QJsonObject &flag)
{
    const auto name = flag.value(QStringLiteral("name")).toString();
    for (const auto &field :
         completion::splitCommandUsageFields(flag.value(QStringLiteral("usage"))
                                                 .toString()
                                                 .left(MAX_USAGE_LENGTH)))
    {
        const auto prefix = name + QChar(':');
        if (field.startsWith(prefix, Qt::CaseInsensitive))
        {
            const auto value = field.mid(prefix.size());
            if (value.contains(QChar('<')))
            {
                return value;
            }
        }
    }
    const auto type = flag.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("boolean"))
    {
        return QStringLiteral("<true|false>");
    }
    if (type.contains(QStringLiteral("int")))
    {
        return QStringLiteral("<number>");
    }
    return QStringLiteral("<%1>").arg(name);
}

std::vector<completion::CommandUsageOption> commandOptions(
    const QJsonObject &object, const QString &command)
{
    std::vector<completion::CommandUsageOption> options;
    qsizetype count = 0;
    for (const auto &value : object.value(QStringLiteral("flags")).toArray())
    {
        if (++count > 32)
        {
            break;
        }
        const auto flag = value.toObject();
        const auto name =
            flag.value(QStringLiteral("name")).toString().toLower();
        if (!validCommandName(name) ||
            !hasLeadingCommand(flag.value(QStringLiteral("usage")).toString(),
                               command))
        {
            continue;
        }
        completion::CommandUsageOption option{
            .name = name,
            .valueHint = flagValueHint(flag),
        };
        for (const auto &aliasValue :
             flag.value(QStringLiteral("aliases")).toArray())
        {
            const auto alias = aliasValue.toString().toLower();
            if (option.aliases.size() < 16 && validCommandName(alias))
            {
                option.aliases.push_back(alias);
            }
        }
        options.push_back(std::move(option));
    }
    return options;
}

std::shared_ptr<const completion::CommandUsage> documentedAliasUsage(
    const QJsonObject &object, const QString &alias)
{
    const auto raw = object.value(QStringLiteral("usage")).toString();
    if (hasLeadingCommand(raw, alias))
    {
        auto usage = normalizeUsage(raw, alias);
        usage.options = commandOptions(object, alias);
        return std::make_shared<const completion::CommandUsage>(
            std::move(usage));
    }

    qsizetype count = 0;
    for (const auto &value : object.value(QStringLiteral("examples")).toArray())
    {
        if (++count > 32)
        {
            break;
        }
        const auto input = value.toObject()
                               .value(QStringLiteral("input"))
                               .toString()
                               .left(MAX_USAGE_LENGTH);
        if (!hasLeadingCommand(input, alias))
        {
            continue;
        }
        for (const auto &field : completion::splitCommandUsageFields(
                 removeLeadingCommand(input, alias)))
        {
            if (!field.contains(QChar(':')) && !field.startsWith(QChar('-')))
            {
                return {};
            }
        }
    }
    completion::CommandUsage best;
    count = 0;
    for (const auto &value : object.value(QStringLiteral("flags")).toArray())
    {
        if (++count > 32)
        {
            break;
        }
        const auto rawForm = value.toObject()
                                 .value(QStringLiteral("usage"))
                                 .toString()
                                 .left(MAX_USAGE_LENGTH);
        if (!hasLeadingCommand(rawForm, alias))
        {
            continue;
        }
        auto form = normalizeUsage(rawForm, alias);
        if (form.kind != completion::CommandUsage::Kind::Named ||
            form.fields.size() <= best.fields.size())
        {
            continue;
        }

        for (auto &field : form.fields)
        {
            const auto colon = field.indexOf(QChar(':'));
            const auto name = field.left(colon);
            const auto valueHint = field.mid(colon + 1);
            if (!valueHint.contains(QChar('<')))
            {
                field = name + QStringLiteral(":<%1>").arg(name);
            }
        }
        form.text = form.fields.join(QChar(' '));
        best = std::move(form);
    }
    if (best.fields.isEmpty())
    {
        return {};
    }
    best.options = commandOptions(object, alias);
    return std::make_shared<const completion::CommandUsage>(std::move(best));
}

bool validLogin(const QString &login)
{
    static const QRegularExpression expression(
        QStringLiteral("^[a-z0-9_]{1,25}$"));
    return expression.match(login).hasMatch();
}

bool validInvocation(const QString &text)
{
    if (text.isEmpty() || text.size() > 64 || text.contains(QChar(' ')))
    {
        return false;
    }
    static const QRegularExpression meta(
        QStringLiteral(R"([.\\+*?\[\](){}|^$])"));
    return !meta.match(text).hasMatch();
}

bool usablePrefix(const QString &prefix)
{
    if (!validInvocation(prefix) || prefix.size() > 16)
    {
        return false;
    }
    return prefix != QStringLiteral("/") && prefix != QStringLiteral(".") &&
           prefix != QStringLiteral("@") && prefix != QStringLiteral("$") &&
           !prefix.startsWith(QStringLiteral("$$"));
}

QString literalInvocation(QString trigger)
{
    trigger = trigger.trimmed();
    if (trigger.size() >= 2 && trigger.startsWith(QChar('/')) &&
        trigger.endsWith(QChar('/')))
    {
        trigger = trigger.mid(1, trigger.size() - 2);
    }
    if (!trigger.startsWith(QChar('^')))
    {
        return {};
    }
    trigger = trigger.mid(1);
    if (trigger.endsWith(QChar('$')))
    {
        trigger.chop(1);
    }
    if (!validInvocation(trigger))
    {
        return {};
    }
    return trigger;
}

QString cleanHelp(const QString &raw)
{
    auto text = raw.trimmed().left(MAX_HELP_LENGTH);
    text.replace(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                 QStringLiteral(" "));
    return text.simplified();
}

bool commandExpired(const QJsonObject &object)
{
    const auto expires =
        object.value(QStringLiteral("expires_at")).toString().trimmed();
    if (expires.isEmpty())
    {
        return false;
    }
    const auto when = QDateTime::fromString(expires, Qt::ISODate);
    return when.isValid() && when < QDateTime::currentDateTimeUtc();
}

bool userRestricted(const QJsonObject &object)
{
    const auto ids = object.value(QStringLiteral("user_trigger_ids"));
    return ids.isArray() && !ids.toArray().isEmpty();
}

QString longestPrefix(const QStringList &prefixes, const QString &invocation)
{
    QString matched;
    for (const auto &prefix : prefixes)
    {
        if (prefix.size() > matched.size() &&
            invocation.startsWith(prefix, Qt::CaseInsensitive))
        {
            matched = prefix;
        }
    }
    return matched;
}

void addChannelCommand(PotatChannelRecord &record, const QString &invocation,
                       const QString &help)
{
    if (record.commands.size() >= MAX_CHANNEL_COMMANDS ||
        !validInvocation(invocation))
    {
        return;
    }
    const auto matched = longestPrefix(record.prefixes, invocation);
    PotatChannelCommand command;
    if (!matched.isEmpty() && invocation.size() > matched.size())
    {
        command.prefix = matched;
        command.name = invocation.mid(matched.size());
    }
    else if (matched.isEmpty())
    {
        command.prefix = invocation;
    }
    else
    {
        return;
    }
    const auto key = (command.prefix + command.name).toLower();
    const auto duplicate = std::ranges::any_of(
        record.commands, [&](const PotatChannelCommand &existing) {
            return (existing.prefix + existing.name)
                       .compare(key, Qt::CaseInsensitive) == 0;
        });
    if (duplicate)
    {
        return;
    }
    command.usage = help;
    record.commands.push_back(std::move(command));
}

QStringList readPrefixes(const QJsonValue &value)
{
    QStringList prefixes;
    const auto append = [&](const QString &raw) {
        const auto prefix = raw.trimmed();
        if (prefixes.size() >= MAX_PREFIXES || !usablePrefix(prefix))
        {
            return;
        }
        const auto exists =
            std::ranges::any_of(prefixes, [&](const QString &existing) {
                return existing.compare(prefix, Qt::CaseInsensitive) == 0;
            });
        if (!exists)
        {
            prefixes.push_back(prefix);
        }
    };
    if (value.isArray())
    {
        for (const auto &entry : value.toArray())
        {
            append(entry.toString());
        }
    }
    else if (value.isString())
    {
        append(value.toString());
    }
    if (prefixes.isEmpty())
    {
        prefixes.push_back(QStringLiteral("#"));
    }
    return prefixes;
}

void readCustomCommands(PotatChannelRecord &record, const QJsonArray &commands)
{
    for (const auto &value : commands)
    {
        if (record.commands.size() >= MAX_CHANNEL_COMMANDS)
        {
            break;
        }
        const auto object = value.toObject();
        if (object.isEmpty() ||
            !object.value(QStringLiteral("active")).toBool(false) ||
            commandExpired(object) || userRestricted(object))
        {
            continue;
        }
        const auto help =
            cleanHelp(object.value(QStringLiteral("help")).toString());
        const auto trigger = object.value(QStringLiteral("trigger")).toString();
        if (!trigger.trimmed().isEmpty())
        {
            const auto invocation = literalInvocation(trigger);
            if (!invocation.isEmpty())
            {
                addChannelCommand(record, invocation, help);
            }
            continue;
        }
        auto name = object.value(QStringLiteral("name")).toString().trimmed();
        if (name.startsWith(QStringLiteral("{any}")))
        {
            name = name.mid(QStringLiteral("{any}").size());
            if (!validInvocation(name))
            {
                continue;
            }
            for (const auto &prefix : record.prefixes)
            {
                addChannelCommand(record, prefix + name, help);
            }
            continue;
        }
        if (!name.isEmpty())
        {
            addChannelCommand(record, name, help);
        }
    }
}

}  // namespace

namespace potat::detail {

std::vector<PotatCommand> parseCommands(const QByteArray &payload)
{
    if (payload.isEmpty() || payload.size() > MAX_PAYLOAD_SIZE)
    {
        return {};
    }

    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray())
    {
        return {};
    }

    std::vector<PotatCommand> commands;
    commands.reserve(
        std::min<qsizetype>(document.array().size(), MAX_COMMANDS) * 2);
    std::unordered_set<QString> names;
    qsizetype count = 0;
    qsizetype aliasCount = 0;
    for (const auto &value : document.array())
    {
        if (++count > MAX_COMMANDS)
        {
            break;
        }
        const auto object = value.toObject();
        if (object.isEmpty() ||
            object.value(QStringLiteral("isDisabled")).toBool(false))
        {
            continue;
        }

        const auto name =
            object.value(QStringLiteral("name")).toString().trimmed();
        if (!validCommandName(name) || !names.insert(name.toLower()).second)
        {
            continue;
        }
        auto usage = normalizeUsage(
            object.value(QStringLiteral("usage")).toString(), name);
        usage.options = commandOptions(object, name);
        for (auto &form : usage.forms)
        {
            form.options = usage.options;
        }
        const auto hint =
            std::make_shared<const completion::CommandUsage>(std::move(usage));
        commands.push_back({
            .name = name,
            .usage = hint->text,
            .dynamicUsage =
                hint->kind != completion::CommandUsage::Kind::Static &&
                hint->kind != completion::CommandUsage::Kind::Alternatives,
            .alias = false,
            .argumentHint = hint,
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
            const auto aliasHint = documentedAliasUsage(object, alias);
            commands.push_back({
                .name = alias,
                .usage = aliasHint ? aliasHint->text
                                   : QStringLiteral("Alias for #%1").arg(name),
                .dynamicUsage =
                    aliasHint &&
                    aliasHint->kind != completion::CommandUsage::Kind::Static &&
                    aliasHint->kind !=
                        completion::CommandUsage::Kind::Alternatives,
                .alias = true,
                .argumentHint = aliasHint,
            });
            ++aliasCount;
        }
    }

    std::ranges::stable_sort(commands, [](const auto &left, const auto &right) {
        return left.name.compare(right.name, Qt::CaseInsensitive) < 0;
    });
    return commands;
}

std::optional<PotatChannelRecord> parseChannel(const QByteArray &payload)
{
    if (payload.isEmpty() || payload.size() > MAX_CHANNEL_PAYLOAD_SIZE)
    {
        return std::nullopt;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
    {
        return std::nullopt;
    }
    const auto data = document.object().value(QStringLiteral("data"));
    if (!data.isArray())
    {
        return std::nullopt;
    }

    PotatChannelRecord record;
    if (data.toArray().isEmpty())
    {
        return record;
    }
    const auto channel =
        data.toArray().at(0).toObject().value(QStringLiteral("channel"));
    if (!channel.isObject())
    {
        return record;
    }
    const auto object = channel.toObject();
    if (object.value(QStringLiteral("state")).toString() !=
        QStringLiteral("JOINED"))
    {
        return record;
    }
    record.joined = true;
    record.prefixes = readPrefixes(object.value(QStringLiteral("settings"))
                                       .toObject()
                                       .value(QStringLiteral("prefix")));
    readCustomCommands(record,
                       object.value(QStringLiteral("commands")).toArray());
    return record;
}

}  // namespace potat::detail

void PotatCommands::ensureLoaded()
{
    if (!getSettings()->includePotatCommands)
    {
        return;
    }
    if (!this->cacheChecked_)
    {
        this->loadCache();
    }
    if (this->fetchedThisSession_ || this->requestInFlight_ ||
        this->requestAttempts_ >= 3 ||
        (this->nextRetryAt_.isValid() &&
         QDateTime::currentDateTimeUtc() < this->nextRetryAt_))
    {
        return;
    }
    this->requestCatalog();
}

const std::vector<PotatCommand> &PotatCommands::commands() const
{
    return this->commands_;
}

bool PotatCommands::isLoading() const
{
    return this->requestInFlight_;
}

void PotatCommands::loadCache()
{
    this->cacheChecked_ = true;

    QFile metadata(metadataPath());
    if (metadata.open(QIODevice::ReadOnly))
    {
        const auto object =
            QJsonDocument::fromJson(metadata.read(MAX_METADATA_SIZE + 1))
                .object();
        this->etag_ = object.value(QStringLiteral("etag")).toString();
    }

    QFile cache(cachePath());
    if (!cache.open(QIODevice::ReadOnly))
    {
        return;
    }
    auto commands =
        potat::detail::parseCommands(cache.read(MAX_PAYLOAD_SIZE + 1));
    if (!commands.empty())
    {
        this->commands_ = std::move(commands);
    }
}

void PotatCommands::requestCatalog(bool useEtag)
{
    if (this->requestInFlight_ || this->requestAttempts_ >= 3)
    {
        return;
    }
    this->requestInFlight_ = true;
    ++this->requestAttempts_;

    auto request = NetworkRequest(CATALOG_URL)
                       .header("Accept", "application/json")
                       .timeout(8000)
                       .caller(this);
    if (useEtag && !this->etag_.isEmpty())
    {
        request = std::move(request).header("If-None-Match", this->etag_);
    }
    std::move(request)
        .onSuccess([this](const NetworkResult &result) {
            if (result.status() == 304)
            {
                if (this->commands_.empty())
                {
                    this->etag_.clear();
                    this->retryWithoutEtag_ = true;
                    return;
                }
                this->fetchedThisSession_ = true;
                return;
            }

            auto commands = potat::detail::parseCommands(result.getData());
            if (commands.empty())
            {
                qCWarning(chatterinoApp)
                    << "[Potat] Ignoring an invalid command catalog.";
                const auto delays = std::array{30, 120, 600};
                const auto delay = delays.at(std::min(
                    this->requestAttempts_ - 1, int(delays.size()) - 1));
                this->nextRetryAt_ =
                    QDateTime::currentDateTimeUtc().addSecs(delay);
                return;
            }
            this->commands_ = std::move(commands);
            this->etag_ = QString::fromUtf8(result.etag());
            this->fetchedThisSession_ = true;
            if (this->saveCache(result.getData()))
            {
                this->saveMetadata();
            }
        })
        .onError([this](const NetworkResult &result) {
            const auto delays = std::array{30, 120, 600};
            const auto delay = delays.at(
                std::min(this->requestAttempts_ - 1, int(delays.size()) - 1));
            this->nextRetryAt_ = QDateTime::currentDateTimeUtc().addSecs(delay);
            qCWarning(chatterinoApp)
                << "[Potat] Command catalog request failed:"
                << result.formatError();
        })
        .finally([this] {
            this->requestInFlight_ = false;
            const auto retryWithoutEtag = this->retryWithoutEtag_;
            this->retryWithoutEtag_ = false;
            this->commandsUpdated.invoke();
            if (retryWithoutEtag)
            {
                QTimer::singleShot(0, this, [this] {
                    this->requestCatalog(false);
                });
            }
        })
        .execute();
}

bool PotatCommands::saveCache(const QByteArray &payload) const
{
    QSaveFile file(cachePath());
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(payload) != payload.size())
    {
        return false;
    }
    return file.commit();
}

QString PotatCommands::normalizeLogin(const QString &channelLogin)
{
    const auto login = channelLogin.trimmed().toLower();
    if (!validLogin(login))
    {
        return {};
    }
    return login;
}

PotatCommands::ChannelState *PotatCommands::channelState(
    const QString &channelLogin)
{
    const auto login = normalizeLogin(channelLogin);
    if (login.isEmpty())
    {
        return nullptr;
    }
    return &this->channelStates_[login];
}

const PotatCommands::ChannelState *PotatCommands::channelState(
    const QString &channelLogin) const
{
    const auto login = normalizeLogin(channelLogin);
    if (login.isEmpty())
    {
        return nullptr;
    }
    const auto it = this->channelStates_.constFind(login);
    if (it == this->channelStates_.cend())
    {
        return nullptr;
    }
    return &(*it);
}

void PotatCommands::ensureChannel(const QString &channelLogin)
{
    if (!getSettings()->includePotatCommands)
    {
        return;
    }
    auto *state = this->channelState(channelLogin);
    if (state == nullptr)
    {
        return;
    }
    const auto login = normalizeLogin(channelLogin);
    if (!state->cacheChecked)
    {
        this->loadChannelCache(login);
    }
    if (state->fetchedThisSession || state->requestInFlight ||
        state->requestAttempts >= 3 ||
        (state->nextRetryAt.isValid() &&
         QDateTime::currentDateTimeUtc() < state->nextRetryAt))
    {
        return;
    }
    this->requestChannel(login, true);
}

bool PotatCommands::channelReady(const QString &channelLogin) const
{
    const auto *state = this->channelState(channelLogin);
    return state != nullptr && state->ready;
}

bool PotatCommands::isChannelLoading(const QString &channelLogin) const
{
    const auto *state = this->channelState(channelLogin);
    return state != nullptr && state->requestInFlight;
}

bool PotatCommands::isJoined(const QString &channelLogin) const
{
    const auto *state = this->channelState(channelLogin);
    return state != nullptr && state->ready && state->record.joined;
}

bool PotatCommands::suggestionQuery(const QString &channelLogin,
                                    const QString &query) const
{
    if (query.isEmpty())
    {
        return false;
    }
    const auto *state = this->channelState(channelLogin);
    if (state == nullptr || !state->ready)
    {
        return query.startsWith(QChar('#'));
    }
    if (!state->record.joined)
    {
        return false;
    }
    for (const auto &prefix : state->record.prefixes)
    {
        if (query.startsWith(prefix, Qt::CaseInsensitive))
        {
            return true;
        }
    }
    const bool includeUnprefixed = getSettings()->showPotatUnprefixedCommands;
    for (const auto &command : state->record.commands)
    {
        if (!command.usesChannelPrefix() && !includeUnprefixed)
        {
            continue;
        }
        const auto invocation = command.prefix + command.name;
        if (!invocation.isEmpty() &&
            invocation.startsWith(query, Qt::CaseInsensitive))
        {
            return true;
        }
    }
    return false;
}

QStringList PotatCommands::prefixesFor(const QString &channelLogin) const
{
    const auto *state = this->channelState(channelLogin);
    if (state == nullptr || !state->ready || !state->record.joined)
    {
        return {};
    }
    return state->record.prefixes;
}

const std::vector<PotatChannelCommand> &PotatCommands::channelCommands(
    const QString &channelLogin) const
{
    static const std::vector<PotatChannelCommand> empty;
    const auto *state = this->channelState(channelLogin);
    if (state == nullptr || !state->ready || !state->record.joined)
    {
        return empty;
    }
    return state->record.commands;
}

void PotatCommands::loadChannelCache(const QString &login)
{
    auto *state = this->channelState(login);
    if (state == nullptr)
    {
        return;
    }
    state->cacheChecked = true;
    QFile metadata(getApp()->getPaths().cacheFilePath(
        QStringLiteral("potat-channel-%1.meta.json").arg(login)));
    if (metadata.open(QIODevice::ReadOnly))
    {
        const auto object =
            QJsonDocument::fromJson(metadata.read(MAX_METADATA_SIZE + 1))
                .object();
        if (object.value(QStringLiteral("login")).toString() == login)
        {
            state->etag = object.value(QStringLiteral("etag")).toString();
        }
    }
    QFile cache(getApp()->getPaths().cacheFilePath(
        QStringLiteral("potat-channel-%1.json").arg(login)));
    if (!cache.open(QIODevice::ReadOnly))
    {
        return;
    }
    auto record =
        potat::detail::parseChannel(cache.read(MAX_CHANNEL_PAYLOAD_SIZE + 1));
    if (!record)
    {
        return;
    }
    state->record = std::move(*record);
    state->ready = true;
}

void PotatCommands::requestChannel(const QString &login, bool useEtag)
{
    auto *state = this->channelState(login);
    if (state == nullptr || state->requestInFlight ||
        state->requestAttempts >= 3)
    {
        return;
    }
    state->requestInFlight = true;
    ++state->requestAttempts;

    auto request =
        NetworkRequest(QString::fromLatin1(USERS_URL) +
                       QString::fromUtf8(QUrl::toPercentEncoding(login)))
            .header("Accept", "application/json")
            .timeout(8000)
            .caller(this);
    if (useEtag && !state->etag.isEmpty())
    {
        request = std::move(request).header("If-None-Match", state->etag);
    }
    std::move(request)
        .onSuccess([this, login](const NetworkResult &result) {
            auto *state = this->channelState(login);
            if (state == nullptr)
            {
                return;
            }
            if (result.status() == 304)
            {
                if (!state->ready)
                {
                    state->etag.clear();
                    state->retryWithoutEtag = true;
                    return;
                }
                state->fetchedThisSession = true;
                return;
            }
            if (result.status() == 404)
            {
                state->record = {};
                state->ready = true;
                state->fetchedThisSession = true;
                return;
            }
            auto record = potat::detail::parseChannel(
                result.getData().left(MAX_CHANNEL_PAYLOAD_SIZE + 1));
            if (!record)
            {
                qCWarning(chatterinoApp)
                    << "[Potat] Ignoring an invalid channel record.";
                const auto delays = std::array{30, 120, 600};
                const auto delay = delays.at(std::min(
                    state->requestAttempts - 1, int(delays.size()) - 1));
                state->nextRetryAt =
                    QDateTime::currentDateTimeUtc().addSecs(delay);
                return;
            }
            state->record = std::move(*record);
            state->etag = QString::fromUtf8(result.etag());
            state->ready = true;
            state->fetchedThisSession = true;
            if (this->saveChannelCache(login, result.getData()))
            {
                this->saveChannelMetadata(login);
            }
        })
        .onError([this, login](const NetworkResult &result) {
            auto *state = this->channelState(login);
            if (state == nullptr)
            {
                return;
            }
            if (result.status() == 404)
            {
                state->record = {};
                state->ready = true;
                state->fetchedThisSession = true;
                return;
            }
            const auto delays = std::array{30, 120, 600};
            const auto delay = delays.at(
                std::min(state->requestAttempts - 1, int(delays.size()) - 1));
            state->nextRetryAt = QDateTime::currentDateTimeUtc().addSecs(delay);
            qCWarning(chatterinoApp) << "[Potat] Channel record request failed:"
                                     << result.formatError();
        })
        .finally([this, login] {
            auto *state = this->channelState(login);
            if (state == nullptr)
            {
                return;
            }
            state->requestInFlight = false;
            const auto retryWithoutEtag = state->retryWithoutEtag;
            state->retryWithoutEtag = false;
            this->commandsUpdated.invoke();
            if (retryWithoutEtag)
            {
                QTimer::singleShot(0, this, [this, login] {
                    this->requestChannel(login, false);
                });
            }
        })
        .execute();
}

bool PotatCommands::saveChannelCache(const QString &login,
                                     const QByteArray &payload) const
{
    QSaveFile file(getApp()->getPaths().cacheFilePath(
        QStringLiteral("potat-channel-%1.json").arg(login)));
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(payload) != payload.size())
    {
        return false;
    }
    return file.commit();
}

void PotatCommands::saveChannelMetadata(const QString &login) const
{
    const auto *state = this->channelState(login);
    if (state == nullptr)
    {
        return;
    }
    QSaveFile file(getApp()->getPaths().cacheFilePath(
        QStringLiteral("potat-channel-%1.meta.json").arg(login)));
    if (!file.open(QIODevice::WriteOnly))
    {
        return;
    }
    file.write(QJsonDocument(QJsonObject{{QStringLiteral("etag"), state->etag},
                                         {QStringLiteral("login"), login}})
                   .toJson(QJsonDocument::Compact));
    file.commit();
}

void PotatCommands::saveMetadata() const
{
    QSaveFile file(metadataPath());
    if (!file.open(QIODevice::WriteOnly))
    {
        return;
    }
    file.write(QJsonDocument(QJsonObject{{QStringLiteral("etag"), this->etag_}})
                   .toJson(QJsonDocument::Compact));
    file.commit();
}

}  // namespace chatterino

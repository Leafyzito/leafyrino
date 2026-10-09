#include "providers/potat/PotatCommands.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "singletons/Paths.hpp"
#include "util/QStringHash.hpp"  // IWYU pragma: keep

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>

#include <algorithm>
#include <array>
#include <unordered_set>

namespace chatterino {

namespace {

constexpr auto CATALOG_URL = "https://api.potat.app/help";
constexpr auto CACHE_FILE = "potat-command-catalog.json";
constexpr auto META_FILE = "potat-command-catalog.meta.json";
constexpr qsizetype MAX_PAYLOAD_SIZE = 2 * 1024 * 1024;
constexpr qsizetype MAX_METADATA_SIZE = 4096;
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

}  // namespace potat::detail

void PotatCommands::ensureLoaded()
{
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

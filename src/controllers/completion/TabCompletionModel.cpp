// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/completion/TabCompletionModel.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/completion/sources/CommandSource.hpp"
#include "controllers/completion/sources/EmoteSource.hpp"
#include "controllers/completion/sources/UnifiedSource.hpp"
#include "controllers/completion/sources/UserSource.hpp"
#include "controllers/completion/strategies/ClassicEmoteStrategy.hpp"
#include "controllers/completion/strategies/ClassicUserStrategy.hpp"
#include "controllers/completion/strategies/CommandStrategy.hpp"
#include "controllers/completion/strategies/SmartEmoteStrategy.hpp"
#include "controllers/plugins/LuaUtilities.hpp"
#include "controllers/plugins/Plugin.hpp"
#include "controllers/plugins/PluginController.hpp"
#include "singletons/Settings.hpp"

namespace chatterino {

TabCompletionModel::TabCompletionModel(Channel &channel, QObject *parent)
    : QStringListModel(parent)
    , channel_(channel)
{
}

void TabCompletionModel::updateResults(const QString &query,
                                       const QString &fullTextContent,
                                       int cursorPosition, bool isFirstWord)
{
    this->updateSourceFromQuery(query, isFirstWord);

    if (this->source_)
    {
        this->source_->update(query);

        QStringList results;
#ifdef CHATTERINO_HAVE_PLUGINS

        bool done{};
        std::tie(done, results) =
            getApp()->getPlugins()->updateCustomCompletions(
                query, fullTextContent, cursorPosition, isFirstWord);
        if (done)
        {
            auto uniqueResults = std::unique(results.begin(), results.end());
            results.erase(uniqueResults, results.end());
            this->setStringList(results);
            return;
        }
#endif
        this->source_->addToStringList(results, 0, isFirstWord);
        auto uniqueResults = std::unique(results.begin(), results.end());
        results.erase(uniqueResults, results.end());
        this->setStringList(results);
    }
}

void TabCompletionModel::updateSourceFromQuery(const QString &query,
                                               bool isFirstWord)
{
    auto deducedKind = this->deduceSourceKind(query, isFirstWord);
    if (!deducedKind)
    {

        this->source_ = nullptr;
        return;
    }

    this->source_ = this->buildSource(*deducedKind);
}

std::optional<TabCompletionModel::SourceKind>
    TabCompletionModel::deduceSourceKind(const QString &query,
                                         bool isFirstWord) const
{
    if (query.length() < 2 || !this->channel_.isTwitchOrKickChannel())
    {
        return std::nullopt;
    }

    if (query.startsWith('@'))
    {
        return SourceKind::User;
    }
    else if (query.startsWith(':'))
    {
        return SourceKind::Emote;
    }
    else if (isFirstWord && (query.startsWith('/') || query.startsWith('.')))
    {
        return SourceKind::Command;
    }

    if (isFirstWord)
    {
        if (getSettings()->userCompletionOnlyWithAt)
        {

            return SourceKind::EmoteCommand;
        }

        return SourceKind::EmoteUserCommand;
    }

    if (getSettings()->userCompletionOnlyWithAt)
    {
        return SourceKind::Emote;
    }

    return SourceKind::EmoteUser;
}

std::unique_ptr<completion::Source> TabCompletionModel::buildSource(
    SourceKind kind) const
{
    switch (kind)
    {
        case SourceKind::Emote: {
            return this->buildEmoteSource();
        }
        case SourceKind::User: {
            return this->buildUserSource(true);
        }
        case SourceKind::Command: {
            return this->buildCommandSource();
        }
        case SourceKind::EmoteUser: {
            std::vector<std::unique_ptr<completion::Source>> sources;
            sources.push_back(this->buildEmoteSource());
            sources.push_back(this->buildUserSource(false));

            return std::make_unique<completion::UnifiedSource>(
                std::move(sources));
        }
        case SourceKind::EmoteCommand: {
            std::vector<std::unique_ptr<completion::Source>> sources;
            sources.push_back(this->buildEmoteSource());
            sources.push_back(this->buildCommandSource());

            return std::make_unique<completion::UnifiedSource>(
                std::move(sources));
        }
        case SourceKind::EmoteUserCommand: {
            std::vector<std::unique_ptr<completion::Source>> sources;
            sources.push_back(this->buildEmoteSource());
            sources.push_back(
                this->buildUserSource(false));
            sources.push_back(this->buildCommandSource());

            return std::make_unique<completion::UnifiedSource>(
                std::move(sources));
        }
        default:
            return nullptr;
    }
}

std::unique_ptr<completion::Source> TabCompletionModel::buildEmoteSource() const
{
    if (getSettings()->useSmartEmoteCompletion)
    {
        return std::make_unique<completion::EmoteSource>(
            &this->channel_,
            std::make_unique<completion::SmartTabEmoteStrategy>());
    }

    return std::make_unique<completion::EmoteSource>(
        &this->channel_,
        std::make_unique<completion::ClassicTabEmoteStrategy>());
}

std::unique_ptr<completion::Source> TabCompletionModel::buildUserSource(
    bool prependAt) const
{
    return std::make_unique<completion::UserSource>(
        &this->channel_, std::make_unique<completion::ClassicUserStrategy>(),
        nullptr, prependAt);
}

std::unique_ptr<completion::Source> TabCompletionModel::buildCommandSource()
    const
{
    return std::make_unique<completion::CommandSource>(
        std::make_unique<completion::CommandStrategy>(true), nullptr,
        &this->channel_);
}

}

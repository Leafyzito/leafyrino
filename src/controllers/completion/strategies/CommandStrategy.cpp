// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/completion/strategies/CommandStrategy.hpp"

#include <algorithm>

namespace chatterino::completion {

QString normalizeQuery(const QString &query)
{
    if (query.startsWith('/') || query.startsWith('.'))
    {
        return query.mid(1);
    }

    return query;
}

CommandStrategy::CommandStrategy(bool startsWithOnly)
    : startsWithOnly_(startsWithOnly)
{
}

void CommandStrategy::apply(const std::vector<CommandItem> &items,
                            std::vector<CommandItem> &output,
                            const QString &query) const
{
    QString normalizedQuery = normalizeQuery(query);

    if (this->startsWithOnly_)
    {
        std::copy_if(items.begin(), items.end(),
                     std::back_insert_iterator(output),
                     [&normalizedQuery](const CommandItem &item) {
                         return item.name.startsWith(normalizedQuery,
                                                     Qt::CaseInsensitive);
                     });
    }
    else
    {
        const auto preferredPrefix =
            (query.startsWith('/') || query.startsWith('.')) ? query.at(0)
                                                             : QChar{};
        const auto hasPreferredPrefix = !preferredPrefix.isNull();
        auto effectivePrefix = [](const CommandItem &item) {
            return item.prefix.isEmpty() ? QStringLiteral("/") : item.prefix;
        };
        auto prefixFits = [&](const CommandItem &item) {
            return !hasPreferredPrefix ||
                   item.prefix == QString(preferredPrefix) ||
                   (preferredPrefix == QChar('/') && item.prefix.isEmpty());
        };

        auto addMatches = [&](auto predicate) {
            for (const auto &item : items)
            {
                if (!predicate(item) || !prefixFits(item))
                {
                    continue;
                }

                const auto alreadyAdded = std::any_of(
                    output.begin(), output.end(), [&](const CommandItem &out) {
                        return effectivePrefix(out) == effectivePrefix(item) &&
                               out.name.compare(item.name,
                                                Qt::CaseInsensitive) == 0;
                    });
                if (!alreadyAdded)
                {
                    output.push_back(item);
                }
            }
        };

        auto exactMatch = [&](const CommandItem &item) {
            return item.name.compare(normalizedQuery, Qt::CaseInsensitive) == 0;
        };
        auto prefixMatch = [&](const CommandItem &item) {
            return item.name.startsWith(normalizedQuery, Qt::CaseInsensitive);
        };
        auto containsMatch = [&](const CommandItem &item) {
            return item.name.contains(normalizedQuery, Qt::CaseInsensitive);
        };

        addMatches(exactMatch);
        addMatches(prefixMatch);
        addMatches(containsMatch);
    }
};

}  // namespace chatterino::completion

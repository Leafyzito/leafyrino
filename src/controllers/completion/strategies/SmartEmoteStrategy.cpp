// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/completion/strategies/SmartEmoteStrategy.hpp"

#include "common/QLogging.hpp"
#include "controllers/completion/sources/EmoteSource.hpp"
#include "singletons/Settings.hpp"
#include "util/Helpers.hpp"

#include <Qt>

#include <algorithm>

namespace chatterino::completion {
namespace {

const auto &LOG = chatterinoCompletion;

int costOfEmote(QStringView query, QStringView emote, bool prioritizeUpper)
{
    int score = 0;

    if (prioritizeUpper)
    {

        for (const auto i : emote)
        {
            score += int(!i.isUpper());
        }
    }
    else
    {

        int len = std::min(emote.size(), query.size());
        for (int i = 0; i < len; i++)
        {

            score += query.at(i).isUpper() ^ emote.at(i).isUpper();
        }
    }

    if (score == 0)
    {
        score = -10;
    }

    auto diff = emote.size() - query.size();
    if (diff > 0)
    {

        score += diff * 100;
    }
    return score;
};

void completeEmotes(
    const std::vector<EmoteItem> &items, std::vector<EmoteItem> &output,
    QStringView query, bool ignoreColonForCost, bool ignoreTildeForCost,
    const std::function<bool(EmoteItem, Qt::CaseSensitivity)> &matchingFunction)
{

    bool haveUpper =
        std::any_of(query.begin(), query.end(), [](const QChar &c) {
            return c.isUpper();
        });

    for (const auto &item : items)
    {
        if (matchingFunction(
                item, haveUpper ? Qt::CaseSensitive : Qt::CaseInsensitive))
        {
            output.push_back(item);
        }
    }

    bool prioritizeUpper = false;

    if (output.empty())
    {
        if (!haveUpper)
        {

            return;
        }

        prioritizeUpper = true;

        for (const auto &item : items)
        {
            if (matchingFunction(item, Qt::CaseInsensitive))
            {
                output.push_back(item);
            }
        }
        if (output.empty())
        {

            return;
        }
    }

    std::sort(output.begin(), output.end(),
              [query, prioritizeUpper, ignoreColonForCost, ignoreTildeForCost](
                  const EmoteItem &a, const EmoteItem &b) -> bool {
                  auto tempA = a.searchName;
                  auto tempB = b.searchName;
                  if (ignoreColonForCost && tempA.startsWith(":"))
                  {
                      tempA = tempA.mid(1);
                  }
                  if (ignoreTildeForCost && tempA.startsWith("~"))
                  {
                      tempA = tempA.mid(1);
                  }
                  if (ignoreColonForCost && tempB.startsWith(":"))
                  {
                      tempB = tempB.mid(1);
                  }
                  if (ignoreTildeForCost && tempB.startsWith("~"))
                  {
                      tempB = tempB.mid(1);
                  }

                  auto costA = costOfEmote(query, tempA, prioritizeUpper);
                  auto costB = costOfEmote(query, tempB, prioritizeUpper);
                  if (costA == costB)
                  {

                      return QString::compare(tempA, tempB,
                                              Qt::CaseInsensitive) < 0;
                  }

                  return costA < costB;
              });
}
}

void SmartEmoteStrategy::apply(const std::vector<EmoteItem> &items,
                               std::vector<EmoteItem> &output,
                               const QString &query) const
{
    qCDebug(LOG) << "SmartEmoteStrategy apply" << query;
    std::vector<EmoteItem> filteredItems = items;
    QString normalizedQuery = query;
    bool ignoreColonForCost = false;
    bool zeroWidthOnly = false;
    if (normalizedQuery.startsWith(':'))
    {
        normalizedQuery = normalizedQuery.mid(1);
        ignoreColonForCost = true;
    }
    if (normalizedQuery.startsWith('~'))
    {
        normalizedQuery = normalizedQuery.mid(1);
        zeroWidthOnly = true;

        auto [first, last] = std::ranges::remove_if(
            filteredItems, [](const EmoteItem &emoteItem) {
                return !emoteItem.emote->zeroWidth;
            });
        filteredItems.erase(first, last);
    }

    completeEmotes(filteredItems, output, normalizedQuery, ignoreColonForCost,
                   zeroWidthOnly,
                   [normalizedQuery](const EmoteItem &left,
                                     Qt::CaseSensitivity caseHandling) {
                       return left.searchName.contains(normalizedQuery,
                                                       caseHandling);
                   });
}

void SmartTabEmoteStrategy::apply(const std::vector<EmoteItem> &items,
                                  std::vector<EmoteItem> &output,
                                  const QString &query) const
{
    qCDebug(LOG) << "SmartTabEmoteStrategy apply" << query;
    bool colonStart = query.startsWith(':');
    QStringView normalizedQuery = query;
    if (colonStart)
    {

        normalizedQuery = normalizedQuery.mid(1);
    }

    completeEmotes(
        items, output, normalizedQuery, false, false,
        [&](const EmoteItem &item, Qt::CaseSensitivity caseHandling) -> bool {
            QStringView itemQuery;
            if (item.isEmoji)
            {
                if (colonStart)
                {
                    itemQuery = normalizedQuery;
                }
                else
                {
                    return false;
                }
            }
            else
            {
                itemQuery = query;
            }

            return startsWithOrContains(
                item.searchName, itemQuery, caseHandling,
                getSettings()->prefixOnlyEmoteCompletion);
        });
}

}

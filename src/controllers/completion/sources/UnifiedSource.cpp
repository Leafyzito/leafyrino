// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/completion/sources/UnifiedSource.hpp"

#include "widgets/listview/GenericListModel.hpp"

namespace chatterino::completion {

UnifiedSource::UnifiedSource(std::vector<std::unique_ptr<Source>> sources)
    : sources_(std::move(sources))
{
}

void UnifiedSource::update(const QString &query)
{
    for (const auto &source : this->sources_)
    {
        source->update(query);
    }
}

void UnifiedSource::addToListModel(GenericListModel &model,
                                   size_t maxCount) const
{
    if (maxCount == 0)
    {
        for (const auto &source : this->sources_)
        {
            source->addToListModel(model, 0);
        }
        return;
    }

    int startingSize = model.rowCount();
    int used = 0;

    for (const auto &source : this->sources_)
    {
        source->addToListModel(model, maxCount - used);

        used = model.rowCount() - startingSize;
        if (used >= static_cast<int>(maxCount))
        {
            break;
        }
    }
}

void UnifiedSource::addToStringList(QStringList &list, size_t maxCount,
                                    bool isFirstWord) const
{
    if (maxCount == 0)
    {
        for (const auto &source : this->sources_)
        {
            source->addToStringList(list, 0, isFirstWord);
        }
        return;
    }

    auto startingSize = list.size();
    QStringList::size_type used = 0;

    for (const auto &source : this->sources_)
    {
        source->addToStringList(list, maxCount - used, isFirstWord);

        used = list.size() - startingSize;
        if (used >= static_cast<QStringList::size_type>(maxCount))
        {
            break;
        }
    }
}

}  // namespace chatterino::completion

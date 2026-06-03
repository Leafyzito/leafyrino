// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QStringList>

namespace chatterino {
class GenericListModel;
}

namespace chatterino::completion {

class Source
{
public:
    virtual ~Source() = default;

    virtual void update(const QString &query) = 0;

    virtual void addToListModel(GenericListModel &model,
                                size_t maxCount = 0) const = 0;

    virtual void addToStringList(QStringList &list, size_t maxCount = 0,
                                 bool isFirstWord = false) const = 0;
};

};  // namespace chatterino::completion

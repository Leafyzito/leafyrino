// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/listview/GenericListModel.hpp"

#include <QObject>
#include <QString>

namespace chatterino {

namespace completion {
class Source;
}

enum class CompletionKind {
    Emote,
    User,
};

class CompletionModel final : public GenericListModel
{
public:
    explicit CompletionModel(QObject *parent);

    void setSource(std::unique_ptr<completion::Source> source);

    bool hasSource() const;

    void updateResults(const QString &query, size_t maxCount = 0);

private:
    std::unique_ptr<completion::Source> source_{};
};

};  // namespace chatterino

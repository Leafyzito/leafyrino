// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"
#include "controllers/completion/sources/Source.hpp"
#include "controllers/completion/strategies/Strategy.hpp"
#include "messages/Emote.hpp"

#include <QString>

#include <functional>
#include <memory>
#include <vector>

namespace chatterino::completion {

struct EmoteItem {
    EmotePtr emote{};

    QString searchName{};

    QString tabCompletionName{};

    QString displayName{};

    QString providerName{};

    bool isEmoji{};
};

class EmoteSource : public Source
{
public:
    using ActionCallback = std::function<void(const QString &)>;
    using EmoteStrategy = Strategy<EmoteItem>;

    EmoteSource(const Channel *channel, std::unique_ptr<EmoteStrategy> strategy,
                ActionCallback callback = nullptr);

    void update(const QString &query) override;
    void addToListModel(GenericListModel &model,
                        size_t maxCount = 0) const override;
    void addToStringList(QStringList &list, size_t maxCount = 0,
                         bool isFirstWord = false) const override;

    const std::vector<EmoteItem> &output() const;

private:
    void initializeFromChannel(const Channel *channel);

    std::unique_ptr<EmoteStrategy> strategy_;
    ActionCallback callback_;

    std::vector<EmoteItem> items_{};
    std::vector<EmoteItem> output_{};
};

}  // namespace chatterino::completion

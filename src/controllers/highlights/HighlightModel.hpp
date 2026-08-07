// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/SignalVectorModel.hpp"

#include <QObject>

namespace chatterino {

class HighlightPhrase;

class HighlightModel : public SignalVectorModel<HighlightPhrase>
{
public:
    explicit HighlightModel(QObject *parent);

    enum Column {
        Pattern = 0,
        ShowInMentions = 1,
        FlashTaskbar = 2,
        UseRegex = 3,
        CaseSensitive = 4,
        PlaySound = 5,
        SoundPath = 6,
        Color = 7,
        COUNT
    };

    enum HighlightRowIndexes {
        SelfHighlightRow = 0,
        WhisperRow = 1,
        SubRow = 2,
        FollowRow = 3,
        RedeemedRow = 4,
        FirstMessageRow = 5,
        ThreadMessageRow = 6,
        AutomodRow = 7,
        WatchStreakRow = 8,
        AnnouncementRow = 9,
        ColoredAnnouncementRow = 10,
    };

    enum UserHighlightRowIndexes {
        SelfMessageRow = 0,
    };

protected:
    HighlightPhrase getItemFromRow(std::vector<QStandardItem *> &row,
                                   const HighlightPhrase &original) override;

    void getRowFromItem(const HighlightPhrase &item,
                        std::vector<QStandardItem *> &row) override;

    void afterInit() override;

    void customRowSetData(const std::vector<QStandardItem *> &row, int column,
                          const QVariant &value, int role,
                          int rowIndex) override;
};

}  // namespace chatterino

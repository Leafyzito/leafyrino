// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/highlights/UserHighlightModel.hpp"

#include "Application.hpp"
#include "controllers/highlights/HighlightPhrase.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/StandardItemHelper.hpp"

namespace chatterino {

UserHighlightModel::UserHighlightModel(QObject *parent)
    : SignalVectorModel<HighlightPhrase>(Column::COUNT, parent)
{
}

HighlightPhrase UserHighlightModel::getItemFromRow(
    std::vector<QStandardItem *> &row, const HighlightPhrase &original)
{
    auto highlightColor = original.getColor();
    *highlightColor =
        row[Column::Color]->data(Qt::DecorationRole).value<QColor>();

    return HighlightPhrase{
        row[Column::Pattern]->data(Qt::DisplayRole).toString().trimmed(),
        row[Column::ShowInMentions]->data(Qt::CheckStateRole).toBool(),
        row[Column::FlashTaskbar]->data(Qt::CheckStateRole).toBool(),
        row[Column::PlaySound]->data(Qt::CheckStateRole).toBool(),
        row[Column::UseRegex]->data(Qt::CheckStateRole).toBool(),
        row[Column::CaseSensitive]->data(Qt::CheckStateRole).toBool(),
        row[Column::SoundPath]->data(Qt::UserRole).toString(),
        highlightColor};
}

void UserHighlightModel::afterInit()
{
    std::vector<QStandardItem *> messagesRow = this->createRow();
    setBoolItem(messagesRow[Column::Pattern],
                getSettings()->enableSelfMessageHighlight.getValue(), true,
                false);
    messagesRow[Column::Pattern]->setData("Your messages (automatic)",
                                          Qt::DisplayRole);
    setBoolItem(messagesRow[Column::ShowInMentions],
                getSettings()->showSelfMessageHighlightInMentions.getValue(),
                true, false);
    messagesRow[Column::FlashTaskbar]->setFlags({});
    messagesRow[Column::PlaySound]->setFlags({});
    messagesRow[Column::UseRegex]->setFlags({});
    messagesRow[Column::CaseSensitive]->setFlags({});
    messagesRow[Column::SoundPath]->setFlags({});

    auto selfColor =
        ColorProvider::instance().color(ColorType::SelfMessageHighlight);
    setColorItem(messagesRow[Column::Color], *selfColor, false);

    this->insertCustomRow(
        messagesRow, HighlightModel::UserHighlightRowIndexes::SelfMessageRow);
}

void UserHighlightModel::customRowSetData(
    const std::vector<QStandardItem *> &row, int column, const QVariant &value,
    int role, int rowIndex)
{
    switch (column)
    {
        case Column::Pattern: {
            if (role == Qt::CheckStateRole)
            {
                if (rowIndex ==
                    HighlightModel::UserHighlightRowIndexes::SelfMessageRow)
                {
                    getSettings()->enableSelfMessageHighlight.setValue(
                        value.toBool());
                }
            }
        }
        break;
        case Column::ShowInMentions: {
            if (role == Qt::CheckStateRole)
            {
                if (rowIndex ==
                    HighlightModel::UserHighlightRowIndexes::SelfMessageRow)
                {
                    getSettings()->showSelfMessageHighlightInMentions.setValue(
                        value.toBool());
                }
            }
        }
        break;
        case Column::Color: {
            if (role == Qt::DecorationRole)
            {
                auto colorName = value.value<QColor>().name(QColor::HexArgb);
                if (rowIndex ==
                    HighlightModel::UserHighlightRowIndexes::SelfMessageRow)
                {
                    getSettings()->selfMessageHighlightColor.setValue(
                        colorName);
                }
            }
        }
        break;
    }

    getApp()->getWindows()->forceLayoutChannelViews();
}

void UserHighlightModel::getRowFromItem(const HighlightPhrase &item,
                                        std::vector<QStandardItem *> &row)
{
    setStringItem(row[Column::Pattern], item.getPattern());
    setBoolItem(row[Column::ShowInMentions], item.showInMentions());
    setBoolItem(row[Column::FlashTaskbar], item.hasAlert());
    setBoolItem(row[Column::PlaySound], item.hasSound());
    setBoolItem(row[Column::UseRegex], item.isRegex());
    setBoolItem(row[Column::CaseSensitive], item.isCaseSensitive());
    setFilePathItem(row[Column::SoundPath], item.getSoundUrl());
    setColorItem(row[Column::Color], *item.getColor());
}

}  // namespace chatterino

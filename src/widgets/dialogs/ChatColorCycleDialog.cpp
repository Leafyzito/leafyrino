// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/ChatColorCycleDialog.hpp"

#include "singletons/Settings.hpp"
#include "util/Twitch.hpp"
#include "widgets/dialogs/ColorPickerDialog.hpp"

#include <QCheckBox>
#include <QColor>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

namespace chatterino {

namespace {

constexpr int TWITCH_COLOR_COLUMNS = 3;

QIcon colorSwatch(const QColor &color)
{
    QPixmap pixmap(16, 16);
    pixmap.fill(color);
    return {pixmap};
}

}  // namespace

ChatColorCycleDialog::ChatColorCycleDialog(QWidget *parent)
    : QDialog(parent)
{
    this->setWindowTitle(QStringLiteral("Chat colors"));
    this->setModal(true);
    this->setMinimumSize(440, 460);

    auto *layout = new QVBoxLayout(this);

    auto *description = new QLabel(
        QStringLiteral(
            "After each message you send, your color changes to one of these, "
            "in order or at random depending on \"Next color\". Twitch's "
            "colors work for everyone, custom colors need Turbo or Prime. "
            "Rainbow doesn't use this list."),
        this);
    description->setWordWrap(true);
    layout->addWidget(description);

    const auto selected = getSettings()->chatColorCycleColors.getValue();

    auto *twitchGroup = new QGroupBox(QStringLiteral("Twitch colors"), this);
    auto *grid = new QGridLayout(twitchGroup);
    // the default holds every twitch color, sorted by hue
    const auto twitchColors =
        getSettings()->chatColorCycleColors.getDefaultValue();
    for (qsizetype i = 0; i < twitchColors.size(); ++i)
    {
        const auto &name = twitchColors.at(i);
        auto *checkBox =
            new QCheckBox(formatHelixColorLabel(name), twitchGroup);
        checkBox->setIcon(colorSwatch(QColor(helixColorDisplayHex(name))));
        checkBox->setChecked(selected.contains(name));
        QObject::connect(checkBox, &QCheckBox::toggled, this, [this] {
            this->save();
        });
        grid->addWidget(checkBox, static_cast<int>(i) / TWITCH_COLOR_COLUMNS,
                        static_cast<int>(i) % TWITCH_COLOR_COLUMNS);
        this->twitchColors_.emplace_back(name, checkBox);
    }
    layout->addWidget(twitchGroup);

    auto *customGroup =
        new QGroupBox(QStringLiteral("Custom colors (Turbo/Prime)"), this);
    auto *customLayout = new QVBoxLayout(customGroup);

    this->customColors_ = new QListWidget(customGroup);
    this->customColors_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (const auto &color : selected)
    {
        if (color.startsWith('#'))
        {
            this->addCustomColor(QColor(color));
        }
    }
    customLayout->addWidget(this->customColors_, 1);

    auto *customButtons = new QHBoxLayout;
    auto *addButton = new QPushButton(QStringLiteral("Add..."), customGroup);
    auto *removeButton = new QPushButton(QStringLiteral("Remove"), customGroup);
    customButtons->addWidget(addButton);
    customButtons->addWidget(removeButton);
    customButtons->addStretch(1);
    customLayout->addLayout(customButtons);

    QObject::connect(addButton, &QPushButton::clicked, this, [this] {
        auto *picker = new ColorPickerDialog(QColor("#9146FF"), this);
        QObject::connect(picker, &ColorPickerDialog::colorConfirmed, this,
                         [this](const QColor &color) {
                             if (color.isValid())
                             {
                                 this->addCustomColor(color);
                                 this->save();
                             }
                         });
        picker->show();
    });
    QObject::connect(removeButton, &QPushButton::clicked, this, [this] {
        qDeleteAll(this->customColors_->selectedItems());
        this->save();
    });

    layout->addWidget(customGroup, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                     &QDialog::reject);
    layout->addWidget(buttons);
}

void ChatColorCycleDialog::addCustomColor(const QColor &color)
{
    const auto hex = color.name(QColor::HexRgb).toUpper();
    if (!this->customColors_->findItems(hex, Qt::MatchExactly).isEmpty())
    {
        return;
    }

    new QListWidgetItem(colorSwatch(color), hex, this->customColors_);
}

void ChatColorCycleDialog::save()
{
    QStringList colors;
    for (const auto &[name, checkBox] : this->twitchColors_)
    {
        if (checkBox->isChecked())
        {
            colors.append(name);
        }
    }
    for (int i = 0; i < this->customColors_->count(); ++i)
    {
        colors.append(this->customColors_->item(i)->text());
    }

    getSettings()->chatColorCycleColors.setValue(colors);
}

}  // namespace chatterino

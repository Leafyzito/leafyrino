// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/BackupsPage.hpp"

#include "Application.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Restart.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>

using namespace Qt::Literals;

namespace {

using namespace chatterino;

QLabel *makeDescription(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    return label;
}

QGridLayout *makeCategoryGrid(QWidget *parent,
                              QHash<QString, QCheckBox *> &checks)
{
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(20);
    int index = 0;
    for (const auto &category : settingstransfer::categories())
    {
        auto *check = new QCheckBox(category.name, parent);
        check->setToolTip(category.description);
        checks.insert(category.id, check);
        grid->addWidget(check, index / 2, index % 2);
        ++index;
    }
    return grid;
}

QSet<QString> selectedCategories(const QHash<QString, QCheckBox *> &checks)
{
    QSet<QString> selected;
    for (auto it = checks.begin(); it != checks.end(); ++it)
    {
        if (it.value()->isChecked())
        {
            selected.insert(it.key());
        }
    }
    return selected;
}

/// Writes everything to disk, so the settings files are up to date
void saveEverything()
{
    getApp()->getHotkeys()->save();
    getApp()->getCommands()->save();
    getApp()->getWindows()->save();
    getSettings()->requestSave();
}

QString settingsDirectory()
{
    return getApp()->getPaths().settingsDirectory;
}

QString documentsDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
}

void restartAfterStaging(QWidget *parent)
{
    if (restartApplication())
    {
        return;
    }
    QMessageBox::information(
        parent, u"Leafyrino - Restart needed"_s,
        u"Leafyrino couldn't restart by itself. Close it and open it again to "
        u"finish. Settings you change until then will be lost."_s);
}

const QString OTHER_INSTANCES_NOTE =
    u"Close any other running Leafyrino first, or it might save over this."_s;

}  // namespace

namespace chatterino {

BackupsPage::BackupsPage()
{
    auto *layout = new QVBoxLayout(this);
    this->tabs_ = new QTabWidget(this);
    this->tabs_->addTab(this->createExportTab(), u"Export"_s);
    this->tabs_->addTab(this->createImportTab(), u"Import"_s);
    this->recoveryTab_ = this->createRecoveryTab();
    this->tabs_->addTab(this->recoveryTab_, u"Recovery"_s);
    layout->addWidget(this->tabs_);

    // reading all the backups takes a moment, so only once they're looked at
    QObject::connect(this->tabs_, &QTabWidget::currentChanged, this,
                     [this](int index) {
                         if (this->tabs_->widget(index) == this->recoveryTab_)
                         {
                             this->refreshRecovery();
                         }
                     });
}

void BackupsPage::onShow()
{
    if (this->tabs_->currentWidget() == this->recoveryTab_)
    {
        this->refreshRecovery();
    }
}

QWidget *BackupsPage::createExportTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(makeDescription(
        u"Save your settings to a file, to keep as a backup or to move them to "
        u"another computer. Accounts, login details, secrets, and paths to "
        u"files on this computer are never included."_s,
        page));

    auto *box = new QGroupBox(u"Include"_s, page);
    auto *boxLayout = new QVBoxLayout(box);
    auto *everything = new QCheckBox(u"Everything"_s, box);
    boxLayout->addWidget(everything);
    boxLayout->addLayout(makeCategoryGrid(box, this->exportChecks_));
    QObject::connect(everything, &QCheckBox::toggled, this,
                     [this](bool checked) {
                         for (auto *check : this->exportChecks_)
                         {
                             if (checked)
                             {
                                 check->setChecked(true);
                             }
                             check->setEnabled(!checked);
                         }
                     });
    everything->setChecked(true);
    layout->addWidget(box);

    auto *exportButton = new QPushButton(u"Export to file..."_s, page);
    QObject::connect(exportButton, &QPushButton::clicked, this,
                     &BackupsPage::exportToFile);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(exportButton);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    this->exportStatus_ = makeDescription({}, page);
    layout->addWidget(this->exportStatus_);
    layout->addStretch(1);
    return page;
}

QWidget *BackupsPage::createImportTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(makeDescription(
        u"Import settings from a file that was exported from Leafyrino or "
        u"Moltorino."_s,
        page));

    auto *fileRow = new QHBoxLayout;
    auto *chooseButton = new QPushButton(u"Choose file..."_s, page);
    QObject::connect(chooseButton, &QPushButton::clicked, this,
                     &BackupsPage::chooseImportFile);
    this->importFile_ = makeDescription({}, page);
    fileRow->addWidget(chooseButton);
    fileRow->addWidget(this->importFile_, 1);
    layout->addLayout(fileRow);

    auto *box = new QGroupBox(u"Import"_s, page);
    auto *boxLayout = new QVBoxLayout(box);
    boxLayout->addLayout(makeCategoryGrid(box, this->importChecks_));
    for (auto *check : this->importChecks_)
    {
        check->setEnabled(false);
        QObject::connect(check, &QCheckBox::toggled, this,
                         &BackupsPage::updateImportActions);
    }
    this->mergeLists_ = new QCheckBox(
        u"Merge lists and keep entries that are already here"_s, box);
    this->mergeLists_->setToolTip(
        u"Otherwise, lists like highlights and commands are replaced by the "
        u"ones in the file."_s);
    this->mergeLists_->setChecked(true);
    boxLayout->addWidget(this->mergeLists_);
    layout->addWidget(box);

    this->importStatus_ =
        makeDescription(u"Choose a file to see what's in it."_s, page);
    layout->addWidget(this->importStatus_);

    this->applyNowButton_ = new QPushButton(u"Apply now"_s, page);
    this->applyNowButton_->setToolTip(
        u"Only appearance settings can be applied without restarting."_s);
    this->applyNowButton_->setEnabled(false);
    QObject::connect(this->applyNowButton_, &QPushButton::clicked, this,
                     &BackupsPage::applyImportNow);
    this->importButton_ = new QPushButton(u"Import and restart"_s, page);
    this->importButton_->setEnabled(false);
    QObject::connect(this->importButton_, &QPushButton::clicked, this,
                     &BackupsPage::importAndRestart);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(this->applyNowButton_);
    buttons->addWidget(this->importButton_);
    buttons->addStretch(1);
    layout->addLayout(buttons);
    layout->addStretch(1);
    return page;
}

QWidget *BackupsPage::createRecoveryTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(makeDescription(
        u"Leafyrino keeps backups of these files every time it saves them. "
        u"Choose a backup for each one you want to restore. Copies of your "
        u"current files are kept, so a restore can be undone from here too."_s,
        page));

    auto *box = new QGroupBox(u"Backups"_s, page);
    auto *grid = new QGridLayout(box);
    grid->setColumnStretch(1, 1);
    grid->setHorizontalSpacing(12);
    int gridRow = 0;
    for (const auto &target : settingstransfer::recoveryTargets())
    {
        RecoveryRow row{
            .target = target,
            .selected = new QCheckBox(target.name, box),
            .backup = new QComboBox(box),
            .details = makeDescription({}, box),
        };
        row.selected->setEnabled(false);
        row.backup->setEnabled(false);
        row.details->setStyleSheet(u"color: #bbb"_s);
        grid->addWidget(row.selected, gridRow, 0);
        grid->addWidget(row.backup, gridRow, 1);
        grid->addWidget(row.details, gridRow + 1, 1);
        gridRow += 2;

        const auto index = this->recoveryRows_.size();
        QObject::connect(row.selected, &QCheckBox::toggled, this,
                         [this, index](bool checked) {
                             auto &current = this->recoveryRows_[index];
                             current.backup->setEnabled(
                                 checked && current.backup->count() > 0);
                             this->updateRestoreButton();
                         });
        QObject::connect(row.backup, &QComboBox::currentIndexChanged, this,
                         [this, index] {
                             this->updateRecoveryRow(index);
                         });
        this->recoveryRows_.push_back(row);
    }
    layout->addWidget(box);

    auto *refreshButton = new QPushButton(u"Refresh"_s, page);
    QObject::connect(refreshButton, &QPushButton::clicked, this,
                     &BackupsPage::refreshRecovery);
    this->restoreButton_ = new QPushButton(u"Restore and restart"_s, page);
    this->restoreButton_->setEnabled(false);
    QObject::connect(this->restoreButton_, &QPushButton::clicked, this,
                     &BackupsPage::restoreSelected);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(refreshButton);
    buttons->addWidget(this->restoreButton_);
    buttons->addStretch(1);
    layout->addLayout(buttons);
    layout->addStretch(1);
    return page;
}

void BackupsPage::exportToFile()
{
    const auto selected = selectedCategories(this->exportChecks_);
    if (selected.isEmpty())
    {
        this->exportStatus_->setText(u"Choose something to export."_s);
        return;
    }

    saveEverything();
    auto contents =
        settingstransfer::exportSettings(settingsDirectory(), selected);
    if (!contents)
    {
        QMessageBox::warning(this, u"Leafyrino - Export failed"_s,
                             contents.error());
        return;
    }

    const auto fileName = u"leafyrino-settings-%1.json"_s.arg(
        QDate::currentDate().toString(Qt::ISODate));
    const auto path = QFileDialog::getSaveFileName(
        this, u"Export settings"_s,
        QDir(documentsDirectory()).filePath(fileName),
        u"Settings files (*.json)"_s);
    if (path.isEmpty())
    {
        return;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(*contents) != contents->size() || !file.commit())
    {
        QMessageBox::warning(
            this, u"Leafyrino - Export failed"_s,
            u"Couldn't write %1."_s.arg(QFileInfo(path).fileName()));
        return;
    }
    this->exportStatus_->setText(
        u"Exported to %1."_s.arg(QFileInfo(path).fileName()));
}

void BackupsPage::chooseImportFile()
{
    const auto path = QFileDialog::getOpenFileName(
        this, u"Import settings"_s, documentsDirectory(),
        u"Settings files (*.json);;All files (*)"_s);
    if (path.isEmpty())
    {
        return;
    }

    this->bundle_.reset();
    this->importFile_->setText(QFileInfo(path).fileName());
    for (auto *check : this->importChecks_)
    {
        const QSignalBlocker blocker(check);
        check->setChecked(false);
        check->setEnabled(false);
    }

    const auto load = [&]() -> ExpectedStr<settingstransfer::Bundle> {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            return makeUnexpected(u"Couldn't read that file."_s);
        }
        return settingstransfer::parseBundle(
            file.read(settingstransfer::MAX_FILE_SIZE + 1));
    };
    auto bundle = load();
    if (!bundle)
    {
        this->importStatus_->setText(bundle.error());
        this->updateImportActions();
        return;
    }

    this->bundle_ = std::move(*bundle);
    const auto present = this->bundle_->categories();
    for (auto it = this->importChecks_.begin(); it != this->importChecks_.end();
         ++it)
    {
        const QSignalBlocker blocker(it.value());
        it.value()->setEnabled(present.contains(it.key()));
        it.value()->setChecked(present.contains(it.key()));
    }
    this->updateImportActions();
}

void BackupsPage::updateImportActions()
{
    const auto selected = selectedCategories(this->importChecks_);
    const bool hasSelection = this->bundle_ && !selected.isEmpty();
    const bool canApplyNow =
        hasSelection &&
        settingstransfer::canApplyImportNow(*this->bundle_, selected);
    this->applyNowButton_->setEnabled(canApplyNow);
    this->importButton_->setEnabled(hasSelection);

    if (!this->bundle_)
    {
        return;
    }
    if (!hasSelection)
    {
        this->importStatus_->setText(u"Choose what to import."_s);
    }
    else if (canApplyNow)
    {
        this->importStatus_->setText(
            u"This can be applied without restarting."_s);
    }
    else
    {
        this->importStatus_->setText(
            u"Leafyrino needs to restart to import this."_s);
    }
}

void BackupsPage::applyImportNow()
{
    if (!this->bundle_)
    {
        return;
    }

    saveEverything();
    auto applied = settingstransfer::applyImportNow(
        settingsDirectory(), *this->bundle_,
        selectedCategories(this->importChecks_),
        this->mergeLists_->isChecked());
    if (!applied)
    {
        QMessageBox::warning(this, u"Leafyrino - Import failed"_s,
                             applied.error());
        this->updateImportActions();
        return;
    }

    // so cancelling the settings doesn't undo the import
    getSettings()->saveSnapshot();
    getApp()->getWindows()->forceLayoutChannelViews();
    this->importStatus_->setText(u"Settings imported."_s);
}

void BackupsPage::importAndRestart()
{
    if (!this->bundle_)
    {
        return;
    }
    if (QMessageBox::question(
            this, u"Leafyrino - Import settings"_s,
            u"Import the selected settings and restart Leafyrino now?\n\n"_s %
                OTHER_INSTANCES_NOTE) != QMessageBox::Yes)
    {
        return;
    }

    saveEverything();
    auto staged =
        settingstransfer::stageImport(settingsDirectory(), *this->bundle_,
                                      selectedCategories(this->importChecks_),
                                      this->mergeLists_->isChecked());
    if (!staged)
    {
        QMessageBox::warning(this, u"Leafyrino - Import failed"_s,
                             staged.error());
        return;
    }
    restartAfterStaging(this);
}

void BackupsPage::refreshRecovery()
{
    this->recoveryFiles_ =
        settingstransfer::inspectRecoveryFiles(settingsDirectory());
    for (size_t i = 0; i < this->recoveryRows_.size(); ++i)
    {
        auto &row = this->recoveryRows_[i];
        {
            const QSignalBlocker blocker(row.backup);
            row.backup->clear();
            for (size_t fileIndex = 0; fileIndex < this->recoveryFiles_.size();
                 ++fileIndex)
            {
                const auto &file = this->recoveryFiles_[fileIndex];
                if (file.targetFile != row.target.file || file.isCurrent() ||
                    file.state == settingstransfer::RecoveryState::Damaged ||
                    file.state == settingstransfer::RecoveryState::Unreadable)
                {
                    continue;
                }
                auto label =
                    QLocale().toString(file.lastModified, QLocale::ShortFormat);
                if (file.isRescueCopy())
                {
                    label += u" (before a restore)"_s;
                }
                row.backup->addItem(label, static_cast<int>(fileIndex));
            }
        }

        const bool available = row.backup->count() > 0;
        row.selected->setEnabled(available);
        if (!available)
        {
            row.selected->setChecked(false);
        }
        row.backup->setEnabled(available && row.selected->isChecked());
        this->updateRecoveryRow(i);
    }
    this->updateRestoreButton();
}

void BackupsPage::updateRecoveryRow(size_t index)
{
    const auto &row = this->recoveryRows_[index];
    const auto describe = [](const settingstransfer::RecoveryFile &file) {
        return u"%1, %2"_s.arg(
            file.summary,
            QLocale().formattedDataSize(file.size, 1,
                                        QLocale::DataSizeTraditionalFormat));
    };

    auto current = u"no file"_s;
    for (const auto &file : this->recoveryFiles_)
    {
        if (file.targetFile == row.target.file && file.isCurrent())
        {
            current = describe(file);
            break;
        }
    }

    const auto fileIndex = row.backup->currentData().toInt();
    if (row.backup->count() == 0 || fileIndex < 0 ||
        static_cast<size_t>(fileIndex) >= this->recoveryFiles_.size())
    {
        row.details->setText(
            u"Now: %1\nThere are no backups to restore."_s.arg(current));
        return;
    }
    row.details->setText(u"Now: %1\nBackup: %2"_s.arg(
        current, describe(this->recoveryFiles_[fileIndex])));
}

void BackupsPage::updateRestoreButton()
{
    this->restoreButton_->setEnabled(
        std::ranges::any_of(this->recoveryRows_, [](const auto &row) {
            return row.selected->isChecked() && row.backup->count() > 0;
        }));
}

void BackupsPage::restoreSelected()
{
    std::vector<settingstransfer::RecoveryFile> files;
    QStringList names;
    for (const auto &row : this->recoveryRows_)
    {
        const auto fileIndex = row.backup->currentData().toInt();
        if (!row.selected->isChecked() || row.backup->count() == 0 ||
            fileIndex < 0 ||
            static_cast<size_t>(fileIndex) >= this->recoveryFiles_.size())
        {
            continue;
        }
        files.push_back(this->recoveryFiles_[fileIndex]);
        names.append(row.target.name.toLower());
    }
    if (files.empty())
    {
        return;
    }

    if (QMessageBox::question(
            this, u"Leafyrino - Restore backup"_s,
            u"Restore the backup of your %1 and restart Leafyrino now?\n\n"_s
                    .arg(names.join(u", "_s)) %
                OTHER_INSTANCES_NOTE) != QMessageBox::Yes)
    {
        return;
    }

    auto staged = settingstransfer::stageRecovery(settingsDirectory(), files);
    if (!staged)
    {
        QMessageBox::warning(this, u"Leafyrino - Restore failed"_s,
                             staged.error());
        return;
    }
    restartAfterStaging(this);
}

}  // namespace chatterino

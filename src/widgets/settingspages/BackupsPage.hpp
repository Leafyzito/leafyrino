// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/SettingsTransfer.hpp"
#include "widgets/settingspages/SettingsPage.hpp"

#include <QHash>

#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTabWidget;

namespace chatterino {

/// Exports settings to a file, imports them again, and restores backups of
/// the settings files
class BackupsPage final : public SettingsPage
{
public:
    BackupsPage();

    void onShow() override;

private:
    QWidget *createExportTab();
    QWidget *createImportTab();
    QWidget *createRecoveryTab();

    void exportToFile();
    void chooseImportFile();
    void updateImportActions();
    void applyImportNow();
    void importAndRestart();

    void refreshRecovery();
    void updateRecoveryRow(size_t index);
    void updateRestoreButton();
    void restoreSelected();

    QTabWidget *tabs_{};
    QWidget *recoveryTab_{};

    QHash<QString, QCheckBox *> exportChecks_;
    QLabel *exportStatus_{};

    QHash<QString, QCheckBox *> importChecks_;
    QLabel *importFile_{};
    QLabel *importStatus_{};
    QCheckBox *mergeLists_{};
    QPushButton *applyNowButton_{};
    QPushButton *importButton_{};
    std::optional<settingstransfer::Bundle> bundle_;

    struct RecoveryRow {
        settingstransfer::RecoveryTarget target;
        QCheckBox *selected{};
        QComboBox *backup{};
        QLabel *details{};
    };
    std::vector<RecoveryRow> recoveryRows_;
    std::vector<settingstransfer::RecoveryFile> recoveryFiles_;
    QPushButton *restoreButton_{};
};

}  // namespace chatterino

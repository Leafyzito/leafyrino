// SPDX-FileCopyrightText: 2021 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QDialog>

#include <memory>

namespace Ui {

class EditHotkeyDialog;

}

namespace chatterino {

class Hotkey;

class EditHotkeyDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit EditHotkeyDialog(const std::shared_ptr<Hotkey> data,
                              QWidget *parent = nullptr);
    ~EditHotkeyDialog() final;

    std::shared_ptr<Hotkey> data();

protected Q_SLOTS:

    void afterEdit();

    void updatePossibleActions();

    void updateArgumentsInput();

private:
    void showEditError(QString errorText);
    void setFromHotkey(std::shared_ptr<Hotkey> hotkey);

    Ui::EditHotkeyDialog *ui_;
    std::shared_ptr<Hotkey> data_;

    bool shownSingleKeyWarning = false;
};

}  // namespace chatterino

// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QDialog>

namespace chatterino {

/// Lists the 7TV paints hidden from chat and lets the user unhide them.
class HiddenSeventvPaintsDialog final : public QDialog
{
public:
    explicit HiddenSeventvPaintsDialog(QWidget *parent = nullptr);
};

}  // namespace chatterino

// SPDX-FileCopyrightText: 2026 Contributors to leafyrino
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/dialogs/MoltorinoAuthPage.hpp"

#include <QString>

class QWidget;

namespace chatterino {

void showMoltorinoAuthDialog(QWidget *parent,
                             const QString &windowTitle = QString(),
                             bool includeKickTab = false);

}  // namespace chatterino

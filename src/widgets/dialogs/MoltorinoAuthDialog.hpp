// SPDX-FileCopyrightText: 2026 Contributors to leafyrino
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/moltorino/MoltorinoAuth.hpp"

#include <QString>

class QWidget;

namespace chatterino {

QString formatMoltorinoAuthSummary(const MoltorinoAuthSummary &summary);

void showMoltorinoAuthDialog(QWidget *parent,
                             const QString &windowTitle = QString(),
                             bool includeKickTab = false);

}  // namespace chatterino

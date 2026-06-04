// SPDX-FileCopyrightText: 2026 Contributors to leafyrino
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/moltorino/MoltorinoAuth.hpp"

#include <QString>

class QTabWidget;
class QWidget;

namespace chatterino {

QString formatMoltorinoAuthSummary(const MoltorinoAuthSummary &summary);

QWidget *createMoltorinoAuthLoginPage(QWidget *parent);
QTabWidget *moltorinoAuthLoginPageTabs(QWidget *page);

}  // namespace chatterino

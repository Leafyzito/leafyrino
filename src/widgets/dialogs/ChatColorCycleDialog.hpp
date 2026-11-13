// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QDialog>
#include <QString>

#include <utility>
#include <vector>

class QCheckBox;
class QColor;
class QListWidget;

namespace chatterino {

/// Picks the colors "Change my color after every message" goes through.
class ChatColorCycleDialog final : public QDialog
{
public:
    explicit ChatColorCycleDialog(QWidget *parent = nullptr);

private:
    void addCustomColor(const QColor &color);
    void save();

    std::vector<std::pair<QString, QCheckBox *>> twitchColors_;
    QListWidget *customColors_{};
};

}  // namespace chatterino

// SPDX-FileCopyrightText: 2021 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/hotkeys/HotkeyCategory.hpp"

#include <QKeySequence>
#include <QString>

#include <vector>

namespace chatterino {

class Hotkey
{
public:
    Hotkey(HotkeyCategory category, QKeySequence keySequence, QString action,
           std::vector<QString> arguments, QString name);
    virtual ~Hotkey() = default;

    QString toString() const;

    QString toPortableString() const;

    HotkeyCategory category() const;

    QString action() const;

    bool validAction() const;

    std::vector<QString> arguments() const;

    QString name() const;

    QString getCategory() const;

    const QKeySequence &keySequence() const;

private:
    HotkeyCategory category_;
    QKeySequence keySequence_;
    QString action_;
    std::vector<QString> arguments_;
    QString name_;

    Qt::ShortcutContext getContext() const;

    friend class HotkeyController;
};

}  // namespace chatterino

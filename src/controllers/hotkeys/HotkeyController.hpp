// SPDX-FileCopyrightText: 2021 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/SignalVector.hpp"
#include "controllers/hotkeys/HotkeyCategory.hpp"

#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>

#include <optional>
#include <set>

class QShortcut;

namespace chatterino {

class Hotkey;

class HotkeyModel;

[[nodiscard]] const std::map<HotkeyCategory, HotkeyCategoryData> &
    hotkeyCategories();

[[nodiscard]] QString hotkeyCategoryName(HotkeyCategory category);

[[nodiscard]] QString hotkeyCategoryDisplayName(HotkeyCategory category);

class HotkeyController final
{
public:
    using HotkeyFunction = std::function<QString(std::vector<QString>)>;
    using HotkeyMap = std::map<QString, HotkeyFunction>;

    HotkeyController();
    HotkeyModel *createModel(QObject *parent);

    std::vector<QShortcut *> shortcutsForCategory(HotkeyCategory category,
                                                  HotkeyMap actionMap,
                                                  QWidget *parent);

    void save();
    std::shared_ptr<Hotkey> getHotkeyByName(QString name);

    QKeySequence getDisplaySequence(
        HotkeyCategory category, const QString &action,
        const std::optional<std::vector<QString>> &arguments = {}) const;

    int replaceHotkey(QString oldName, std::shared_ptr<Hotkey> newHotkey);
    std::optional<HotkeyCategory> hotkeyCategoryFromName(QString categoryName);

    [[nodiscard]] bool isDuplicate(std::shared_ptr<Hotkey> hotkey,
                                   QString ignoreNamed);

    pajlada::Signals::NoArgSignal onItemsUpdated;

    void clearRemovedDefaults();

    const std::set<QString> &removedOrDeprecatedHotkeys() const;

private:

    void loadHotkeys();

    void saveHotkeys();

    void addDefaults(std::set<QString> &addedHotkeys);

    void resetToDefaults();

    void tryAddDefault(std::set<QString> &addedHotkeys, HotkeyCategory category,
                       QKeySequence keySequence, QString action,
                       std::vector<QString> args, QString name);

    bool tryRemoveDefault(HotkeyCategory category, QKeySequence keySequence,
                          QString action, std::vector<QString> args,
                          QString name);

    void warnForRemovedHotkeyActions(HotkeyCategory category, QString action,
                                     std::vector<QString> args);

    static void showHotkeyError(const std::shared_ptr<Hotkey> &hotkey,
                                QString warning);

    std::shared_ptr<Hotkey> findLike(
        HotkeyCategory category, const QString &action,
        const std::optional<std::vector<QString>> &arguments = {}) const;

    friend class KeyboardSettingsPage;

    std::set<QString> removedOrDeprecatedHotkeys_;

    SignalVector<std::shared_ptr<Hotkey>> hotkeys_;
    pajlada::Signals::SignalHolder signalHolder_;
};

}

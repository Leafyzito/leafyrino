// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/Expected.hpp"

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>
#include <QString>

#include <cstdint>
#include <vector>

class QByteArray;

/// Exporting settings to a file, importing them again, and restoring the
/// backups that are kept of the settings files
///
/// Imports and restores are staged in the settings directory and written by
/// applyPendingRestore on the next start, before anything reads the files.
namespace chatterino::settingstransfer {

/// Files bigger than this aren't read
inline constexpr qsizetype MAX_FILE_SIZE = 32 * 1024 * 1024;

struct Category {
    QString id;
    QString name;
    QString description;
};

/// All categories, in the order they're shown
const std::vector<Category> &categories();

/// Returns the ID of the category the setting at `path` belongs to
QString categoryForSettingPath(const QString &path);

/// Returns false for settings that must never be exported or imported, like
/// accounts, secrets, local paths, and anything that can start programs
bool isTransferableSettingPath(const QString &path);

/// An exported settings file
struct Bundle {
    /// Category ID -> (setting path -> value)
    QHash<QString, QJsonObject> settings;
    /// File name (e.g. commands.json) -> contents
    QHash<QString, QJsonValue> files;

    /// IDs of the categories that have something to import
    QSet<QString> categories() const;
};

/// Exports the selected categories from the files in `settingsDirectory`
///
/// Save everything to disk first, this only reads the files.
ExpectedStr<QByteArray> exportSettings(const QString &settingsDirectory,
                                       const QSet<QString> &selected);

/// Reads an exported settings file. Settings that can't be imported are left
/// out, and settings are sorted into the categories of this version.
ExpectedStr<Bundle> parseBundle(const QByteArray &contents);

/// Stages the selected categories of `bundle` to be written on the next start
///
/// With `mergeLists`, lists and objects are merged with the current ones
/// instead of replacing them.
ExpectedStr<void> stageImport(const QString &settingsDirectory,
                              const Bundle &bundle,
                              const QSet<QString> &selected, bool mergeLists);

/// Returns true if the selected categories can be applied without restarting
bool canApplyImportNow(const Bundle &bundle, const QSet<QString> &selected);

/// Applies the selected categories right away
///
/// Only works if canApplyImportNow returns true. A rescue copy of
/// settings.json is kept like with a staged import.
ExpectedStr<void> applyImportNow(const QString &settingsDirectory,
                                 const Bundle &bundle,
                                 const QSet<QString> &selected,
                                 bool mergeLists);

enum class RecoveryState : std::uint8_t {
    Healthy,
    Empty,
    Damaged,
    Unreadable,
};

/// A settings file, one of its backups, or a rescue copy
struct RecoveryFile {
    QString path;
    /// The settings file this is a version of (e.g. settings.json)
    QString targetFile;
    QDateTime lastModified;
    qint64 size = 0;
    RecoveryState state = RecoveryState::Unreadable;
    /// What the file contains (e.g. "3 tabs, 5 chats")
    QString summary;

    /// Returns true if this is the file in use, not a backup
    bool isCurrent() const;
    /// Returns true if this is a copy made before a restore or import
    bool isRescueCopy() const;
};

/// The files that can be restored and what they're shown as
struct RecoveryTarget {
    QString file;
    QString name;
};
const std::vector<RecoveryTarget> &recoveryTargets();

/// Lists the restorable settings files and all their backups and rescue
/// copies, newest first
std::vector<RecoveryFile> inspectRecoveryFiles(
    const QString &settingsDirectory);

/// Stages backups to be restored on the next start, at most one per file
ExpectedStr<void> stageRecovery(const QString &settingsDirectory,
                                const std::vector<RecoveryFile> &files);

/// Writes the import or restore staged for this start, if there is one
///
/// Rescue copies of the replaced files are kept. On failure, the files that
/// were already written are rolled back and the staged restore is discarded.
ExpectedStr<void> applyPendingRestore(const QString &settingsDirectory);

QString recoveryStateText(RecoveryState state);

}  // namespace chatterino::settingstransfer

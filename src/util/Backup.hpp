// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/Expected.hpp"

#include <QDateTime>
#include <QString>

#include <filesystem>
#include <vector>

class QJsonValue;

namespace chatterino {
class Paths;
}

namespace chatterino::backup {

enum class BackupState : uint8_t {

    Ok,

    UnableToRead,

    BadContents,
};

struct BackupFile {
    std::filesystem::path path;
    std::filesystem::path dstPath;
    QDateTime lastModified;
    qint64 fileSize = 0;
    BackupState state = BackupState::Ok;
};

struct FileData {

    QString fileName;
    QString directory;

    QString fileKind;

    QString fileDescription;
};

std::vector<BackupFile> findBackupsFor(const QString &directory,
                                       const QString &filename);

void loadWithBackups(const FileData &fileData,
                     const std::function<ExpectedStr<void>()> &load);

}

Q_DECLARE_METATYPE(chatterino::backup::BackupFile);

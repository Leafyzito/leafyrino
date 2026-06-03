// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

#include <optional>

namespace chatterino {

class Paths
{
public:
    Paths();

    QString rootAppDataDirectory;

    QString settingsDirectory;

    QString messageLogDirectory;

    QString miscDirectory;

    QString crashdumpDirectory;

    QString applicationFilePathHash;

    QString twitchProfileAvatars;

    QString pluginsDirectory;

    QString themesDirectory;

    QString dictionariesDirectory;

    QString ipcDirectory;

    bool createFolder(const QString &folderPath);
    [[deprecated("use Modes::instance().portable instead")]] bool isPortable()
        const;

    QString cacheDirectory() const;

    QString cacheFilePath(const QString &fileName) const;

private:
    void initAppFilePathHash();
    void initCheckPortable();
    void initRootDirectory();
    void initSubDirectories();

    std::optional<bool> portable_;

    QString cacheDirectory_;
};

}  // namespace chatterino

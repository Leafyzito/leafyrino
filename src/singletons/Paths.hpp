// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

#include <optional>

namespace chatterino {

class Modes;

class Paths
{
public:
    Paths(const Modes &modes);

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

    QString cacheDirectory() const;

    QString cacheFilePath(const QString &fileName) const;

private:
    void initAppFilePathHash();
    void initCheckPortable();
    void initRootDirectory(const Modes &modes);
    void initSubDirectories();

    std::optional<bool> portable_;

    QString cacheDirectory_;
};

}  // namespace chatterino

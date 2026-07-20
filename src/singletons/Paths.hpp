// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

namespace chatterino {

class Modes;
class Args;

class Paths
{
public:
    Paths(const Args &args, const Modes &modes);

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
    void initRootDirectory(const Args &args, const Modes &modes);
    void initSubDirectories();

    // Directory for cache files. Same as <appDataDirectory>/Misc
    QString cacheDirectory_;
};

}  // namespace chatterino

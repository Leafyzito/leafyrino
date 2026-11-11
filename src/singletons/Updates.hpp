// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <pajlada/signals/signal.hpp>
#include <QString>

#include <memory>
#include <optional>

class QJsonObject;
class QTimer;

namespace chatterino {

class Paths;
class Modes;

class Updates
{
    const Paths &paths;
    const Modes &modes;

public:
    Updates(const Modes &modes_, const Paths &paths_);
    ~Updates();

    Updates(const Updates &) = delete;
    Updates &operator=(const Updates &) = delete;
    Updates(Updates &&) = delete;
    Updates &operator=(Updates &&) = delete;

    enum Status {
        None,
        Searching,
        UpdateAvailable,
        NoUpdateAvailable,
        SearchFailed,
        Downloading,
        DownloadFailed,
        VerifyFailed,
        WriteFileFailed,
        RunUpdaterFailed,
    };

    /// How an update gets installed on this system
    enum class InstallMethod {
        /// Download the Windows installer and run it silently
        Installer,
        /// Download the portable zip and extract it with the portable updater
        Portable,
        /// Open the release page so the user can download the update
        Manual,
    };

    struct Download {
        QString url;
        QString sha256;
    };

    /// The contents of version.json on the nightly release
    struct BuildInfo {
        int build = 0;
        QString commit;
        std::optional<Download> installer;
        std::optional<Download> portable;
    };

    /// Parses version.json. `arch` picks the Windows downloads ("x86-64" or
    /// "arm64"). Returns nothing if the build number is missing.
    static std::optional<BuildInfo> parseBuildInfo(const QJsonObject &root,
                                                   const QString &arch);

    void deleteOldFiles();

    /// Checks for updates now and then every few hours
    void startPeriodicChecks();
    void checkForUpdates();

    int getCurrentBuild() const;
    int getOnlineBuild() const;
    InstallMethod installMethod() const;
    void installUpdates();
    Status getStatus() const;

    static QString portableUpdaterPath(const Paths &paths);
    static const QString &releasePageUrl();

    bool shouldShowUpdateButton() const;
    bool isError() const;
    bool canInstall() const;

    /// Generates the string that the update dialog will show.
    QString buildUpdateAvailableText() const;

    pajlada::Signals::Signal<Status> statusUpdated;

private:
    int currentBuild_;
    std::optional<BuildInfo> online_;
    Status status_ = None;

    std::unique_ptr<QTimer> checkTimer_;

    bool checksEnabled() const;
    bool hasUpdate() const;
    void handleBuildInfo(BuildInfo info);
    void handleSearchFailed();
    void downloadAndRun(const Download &download, InstallMethod method);
    void runDownloaded(const QString &filePath, InstallMethod method);

    void setStatus_(Status status, bool force = false);
};

}  // namespace chatterino

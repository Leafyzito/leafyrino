// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/Updates.hpp"

#include "common/Literals.hpp"
#include "common/Modes.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "common/Version.hpp"
#include "singletons/Paths.hpp"
#include "util/CombinePath.hpp"
#include "util/PostToThread.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStringBuilder>
#include <QtConcurrent>
#include <QTimer>
#include <QUrl>

#ifdef Q_OS_WIN
#    include <Windows.h>
#endif

#include <chrono>

namespace {

using namespace chatterino;
using namespace literals;

// published by .github/workflows/create-installer.yml
const QString VERSION_INFO_URL =
    u"https://github.com/leafyzito/leafyrino/releases/download/nightly-build/version.json"_s;
const QString RELEASE_PAGE_URL =
    u"https://github.com/leafyzito/leafyrino/releases/tag/nightly-build"_s;

constexpr auto CHECK_INTERVAL = std::chrono::hours(4);

#ifdef Q_PROCESSOR_ARM_64
const QString CURRENT_ARCH = u"arm64"_s;
#else
const QString CURRENT_ARCH = u"x86-64"_s;
#endif

std::optional<Updates::Download> parseDownload(const QJsonValue &value)
{
    static const QRegularExpression sha256Regex(u"^[0-9a-f]{64}$"_s);

    const auto obj = value.toObject();
    Updates::Download download{
        .url = obj["url"_L1].toString(),
        .sha256 = obj["sha256"_L1].toString().toLower(),
    };

    const QUrl url(download.url);
    if (!url.isValid() || url.scheme() != u"https"_s ||
        !sha256Regex.match(download.sha256).hasMatch())
    {
        return std::nullopt;
    }

    return download;
}

#ifdef Q_OS_WIN

// keep in sync with the AppId in .CI/chatterino-installer.iss
const QString PER_USER_UNINSTALL_KEY =
    uR"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Uninstall\{F5FE6614-04D4-4D32-8600-0ABA0AC113A4}_is1)"_s;

/// Whether this copy was installed by the installer (and not unpacked from a zip)
bool isInstalled()
{
    return !QDir(QCoreApplication::applicationDirPath())
                .entryList({u"unins*.exe"_s}, QDir::Files)
                .isEmpty();
}

/// Whether the installer was run for the current user only, so updating
/// doesn't need admin rights
bool isPerUserInstall()
{
    QSettings key(PER_USER_UNINSTALL_KEY, QSettings::NativeFormat);
    const auto location = key.value(u"InstallLocation"_s).toString();
    return !location.isEmpty() &&
           QDir(location) == QDir(QCoreApplication::applicationDirPath());
}

bool startInstaller(const QString &installerPath)
{
    QProcess process;
    process.setProgram(installerPath);
    process.setArguments({
        u"/SILENT"_s,
        u"/SUPPRESSMSGBOXES"_s,
        u"/NORESTART"_s,
        u"/SP-"_s,
        // we exit right away, but close us if we're still around. The
        // installer starts the new version once it's done.
        u"/CLOSEAPPLICATIONS"_s,
        u"/NORESTARTAPPLICATIONS"_s,
        isPerUserInstall() ? u"/CURRENTUSER"_s : u"/ALLUSERS"_s,
        u"/DIR="_s %
            QDir::toNativeSeparators(QCoreApplication::applicationDirPath()),
    });
    return process.startDetached();
}

bool startPortableUpdater(const QString &updaterPath, const QString &zipPath)
{
    // The portable updater can only restart "chatterino.exe", so cmd runs it
    // and starts us again once it succeeded. It waits for us to exit on its own.
    const auto command = uR"(/d /s /c ""%1" "%2" && start "" "%3"")"_s.arg(
        QDir::toNativeSeparators(updaterPath),
        QDir::toNativeSeparators(zipPath),
        QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));

    auto cmd = qEnvironmentVariable("ComSpec");
    if (cmd.isEmpty())
    {
        cmd = u"cmd.exe"_s;
    }

    QProcess process;
    process.setProgram(cmd);
    process.setNativeArguments(command);
    process.setWorkingDirectory(QCoreApplication::applicationDirPath());
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) {
            args->flags |= CREATE_NO_WINDOW;
        });
    return process.startDetached();
}

#endif

}  // namespace

namespace chatterino {

Updates::Updates(const Modes &modes_, const Paths &paths_)
    : paths(paths_)
    , modes(modes_)
    , currentBuild_(Version::instance().buildNumber())
{
    qCDebug(chatterinoUpdate) << "init UpdateManager";
}

Updates::~Updates() = default;

std::optional<Updates::BuildInfo> Updates::parseBuildInfo(
    const QJsonObject &root, const QString &arch)
{
    BuildInfo info{
        .build = root["build"_L1].toInt(),
        .commit = root["commit"_L1].toString(),
    };
    if (info.build <= 0)
    {
        return std::nullopt;
    }

    const auto windows = root["windows"_L1].toObject().value(arch).toObject();
    info.installer = parseDownload(windows["installer"_L1]);
    info.portable = parseDownload(windows["portable"_L1]);

    return info;
}

void Updates::deleteOldFiles()
{
    std::ignore = QtConcurrent::run([dir{this->paths.miscDirectory}] {
        {
            auto path = combinePath(dir, "Update.exe");
            if (QFile::exists(path))
            {
                QFile::remove(path);
            }
        }
        {
            auto path = combinePath(dir, "update.zip");
            if (QFile::exists(path))
            {
                QFile::remove(path);
            }
        }
    });
}

bool Updates::checksEnabled() const
{
#ifdef CHATTERINO_DISABLE_UPDATER
    return false;
#else
    // Flatpak is included because the bundle on the release has no repository
    // to update from
    return Version::instance().isSupportedOS() &&
           !this->modes.isExternallyPackaged && this->currentBuild_ > 0;
#endif
}

void Updates::startPeriodicChecks()
{
    if (!this->checksEnabled())
    {
        qCDebug(chatterinoUpdate)
            << "Update checks are disabled for this build (build number:"
            << this->currentBuild_ << ")";
        return;
    }

    this->checkForUpdates();

    if (!this->checkTimer_)
    {
        this->checkTimer_ = std::make_unique<QTimer>();
        QObject::connect(this->checkTimer_.get(), &QTimer::timeout, [this] {
            this->checkForUpdates();
        });
        this->checkTimer_->start(CHECK_INTERVAL);
    }
}

void Updates::checkForUpdates()
{
    if (!this->checksEnabled())
    {
        return;
    }

    // don't get in the way of an update that's being downloaded
    if (this->status_ == Downloading)
    {
        return;
    }

    // keep offering an update we already know about while we look for a newer one
    if (!this->hasUpdate())
    {
        this->setStatus_(Searching);
    }

    qCDebug(chatterinoUpdate) << "Requesting updates from" << VERSION_INFO_URL;
    NetworkRequest(VERSION_INFO_URL)
        .timeout(60000)
        .followRedirects(true)
        .onSuccess([this](const NetworkResult &result) {
            auto info =
                Updates::parseBuildInfo(result.parseJson(), CURRENT_ARCH);
            if (!info)
            {
                qCWarning(chatterinoUpdate)
                    << "Invalid version info:" << result.getData();
                this->handleSearchFailed();
                return;
            }
            this->handleBuildInfo(std::move(*info));
        })
        .onError([this](const NetworkResult &result) {
            qCWarning(chatterinoUpdate)
                << "Failed to check for updates:" << result.formatError();
            this->handleSearchFailed();
        })
        .execute();
}

void Updates::handleBuildInfo(BuildInfo info)
{
    const bool buildChanged =
        !this->online_ || this->online_->build != info.build;
    this->online_ = std::move(info);

    if (this->status_ == Downloading)
    {
        return;
    }

    if (this->hasUpdate())
    {
        qCDebug(chatterinoUpdate)
            << "Build" << this->online_->build << "(" << this->online_->commit
            << ") is available, current build is" << this->currentBuild_;
        // a newer build brings the update button back even if it was dismissed
        this->setStatus_(UpdateAvailable, buildChanged);
    }
    else
    {
        this->setStatus_(NoUpdateAvailable);
    }
}

void Updates::handleSearchFailed()
{
    if (this->status_ == Downloading)
    {
        return;
    }

    this->setStatus_(this->hasUpdate() ? UpdateAvailable : SearchFailed);
}

int Updates::getCurrentBuild() const
{
    return this->currentBuild_;
}

int Updates::getOnlineBuild() const
{
    return this->online_ ? this->online_->build : 0;
}

bool Updates::hasUpdate() const
{
    return this->online_ && this->online_->build > this->currentBuild_;
}

Updates::InstallMethod Updates::installMethod() const
{
    if (!this->online_)
    {
        return InstallMethod::Manual;
    }

#ifdef Q_OS_WIN
    if (this->modes.isPortable)
    {
        if (this->online_->portable &&
            QFile::exists(Updates::portableUpdaterPath(this->paths)))
        {
            return InstallMethod::Portable;
        }
        return InstallMethod::Manual;
    }

    if (this->online_->installer && isInstalled())
    {
        return InstallMethod::Installer;
    }
#endif

    return InstallMethod::Manual;
}

void Updates::installUpdates()
{
    if (!this->canInstall())
    {
        return;
    }

    switch (this->installMethod())
    {
        case InstallMethod::Installer:
            this->downloadAndRun(*this->online_->installer,
                                 InstallMethod::Installer);
            break;

        case InstallMethod::Portable:
            this->downloadAndRun(*this->online_->portable,
                                 InstallMethod::Portable);
            break;

        case InstallMethod::Manual:
            QDesktopServices::openUrl(QUrl(RELEASE_PAGE_URL));
            break;
    }
}

void Updates::downloadAndRun(const Download &download, InstallMethod method)
{
    qCDebug(chatterinoUpdate) << "Downloading update from" << download.url;
    this->setStatus_(Downloading);

    NetworkRequest(download.url)
        .timeout(600000)
        .followRedirects(true)
        .onError([this](const NetworkResult &result) {
            qCWarning(chatterinoUpdate)
                << "Failed to download the update:" << result.formatError();
            this->setStatus_(DownloadFailed);
        })
        .onSuccess([this, download, method](const NetworkResult &result) {
            const auto &data = result.getData();

            const auto hash = QString::fromLatin1(
                QCryptographicHash::hash(data, QCryptographicHash::Sha256)
                    .toHex());
            if (hash != download.sha256)
            {
                qCWarning(chatterinoUpdate)
                    << "Update checksum mismatch - expected" << download.sha256
                    << "got" << hash;
                this->setStatus_(VerifyFailed);
                return;
            }

            const auto filePath =
                combinePath(this->paths.miscDirectory,
                            method == InstallMethod::Portable ? "update.zip"
                                                              : "Update.exe");
            QFile file(filePath);
            if (!file.open(QIODevice::Truncate | QIODevice::WriteOnly) ||
                file.write(data) != data.size())
            {
                qCWarning(chatterinoUpdate)
                    << "Failed to write" << filePath << file.errorString();
                this->setStatus_(WriteFileFailed);
                return;
            }
            file.close();

            this->runDownloaded(filePath, method);
        })
        .execute();
}

void Updates::runDownloaded(const QString &filePath, InstallMethod method)
{
#ifdef Q_OS_WIN
    const bool started =
        method == InstallMethod::Portable
            ? startPortableUpdater(Updates::portableUpdaterPath(this->paths),
                                   filePath)
            : startInstaller(filePath);
    if (!started)
    {
        qCWarning(chatterinoUpdate) << "Failed to start the updater";
        this->setStatus_(RunUpdaterFailed);
        return;
    }

    QApplication::exit(0);
#else
    // only Windows downloads updates
    (void)filePath;
    (void)method;
    this->setStatus_(RunUpdaterFailed);
#endif
}

Updates::Status Updates::getStatus() const
{
    return this->status_;
}

QString Updates::portableUpdaterPath(const Paths &paths)
{
    return combinePath(paths.rootAppDataDirectory,
                       "updater.1/ChatterinoUpdater.exe");
}

const QString &Updates::releasePageUrl()
{
    return RELEASE_PAGE_URL;
}

bool Updates::shouldShowUpdateButton() const
{
    switch (this->getStatus())
    {
        case UpdateAvailable:
        case Downloading:
        case DownloadFailed:
        case VerifyFailed:
        case WriteFileFailed:
        case RunUpdaterFailed:
            return true;

        default:
            return false;
    }
}

bool Updates::isError() const
{
    switch (this->getStatus())
    {
        case SearchFailed:
        case DownloadFailed:
        case VerifyFailed:
        case WriteFileFailed:
        case RunUpdaterFailed:
            return true;

        default:
            return false;
    }
}

bool Updates::canInstall() const
{
    return this->hasUpdate() && this->status_ != Downloading;
}

QString Updates::buildUpdateAvailableText() const
{
    auto text = QString("Build %1 is available (you have build %2).")
                    .arg(this->getOnlineBuild())
                    .arg(this->currentBuild_);

    if (this->installMethod() == InstallMethod::Manual)
    {
        return text % "\n\nDo you want to open the download page?";
    }

    return text % "\n\nInstall it now? Leafyrino will restart.";
}

void Updates::setStatus_(Status status, bool force)
{
    if (this->status_ != status || force)
    {
        this->status_ = status;
        postToThread([this, status] {
            this->statusUpdated.invoke(status);
        });
    }
}

}  // namespace chatterino

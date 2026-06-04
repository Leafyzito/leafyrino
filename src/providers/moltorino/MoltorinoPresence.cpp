// ew no telemetry

#include "providers/moltorino/MoltorinoPresence.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "common/Version.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/moltorino/MoltorinoSupporterBadges.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/PostToThread.hpp"
#include "widgets/Window.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIODevice>
#include <QJsonDocument>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProgressDialog>
#include <QRandomGenerator>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>
#include <QUuid>
#include <QVariant>

#include <algorithm>
#include <memory>
#include <utility>

namespace chatterino {

Q_LOGGING_CATEGORY(chatterinoMoltorinoPresence, "chatterino.moltorino.presence",
                   QtWarningMsg)

namespace {

constexpr auto API_BASE = "https://presence.example.com";
constexpr auto HEARTBEAT_PATH = "/api/client/heartbeat";
constexpr auto BADGE_SOCKET_PATH = "/api/client/badges/ws";
constexpr auto HEARTBEAT_INTERVAL_MS = 30 * 60 * 1000;
constexpr auto BADGE_SOCKET_RECONNECT_BASE_MS = 15000;
constexpr auto BADGE_SOCKET_RECONNECT_MAX_MS = 5 * 60 * 1000;
constexpr auto UPDATE_DOWNLOAD_TIMEOUT_MS = 10 * 60 * 1000;

QString platformKey()
{
#if defined(Q_OS_WIN)
    return "windows";
#elif defined(Q_OS_MACOS)
    return "macos";
#elif defined(Q_OS_LINUX)
    return "linux";
#else
    return QSysInfo::productType();
#endif
}

QUrl apiUrl(const char *path)
{
    return QUrl(QString::fromLatin1(API_BASE) + QString::fromLatin1(path));
}

QUrl badgeSocketUrl()
{
    auto url = apiUrl(BADGE_SOCKET_PATH);
    const auto scheme = url.scheme().toLower();

    if (scheme == "https")
    {
        url.setScheme("wss");
    }
    else if (scheme == "http")
    {
        url.setScheme("ws");
    }

    return url;
}

bool badgeSocketConfigured()
{
    const auto url = badgeSocketUrl();
    return url.isValid() &&
           url.host().compare("presence.example.com", Qt::CaseInsensitive) != 0;
}

QString savedClientId()
{
    QSettings settings(QStringLiteral("Leafyrino"),
                       QStringLiteral("Leafyrino7"));
    auto id = settings.value(QStringLiteral("presence/clientInstanceId"))
                  .toString()
                  .trimmed();

    if (id.isEmpty())
    {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue(QStringLiteral("presence/clientInstanceId"), id);
    }

    return id;
}

bool activityHeartbeatsEnabled()
{
    const auto *settings = getSettings();
    return settings->transmitPresence && settings->sendActivityHeartbeats;
}

bool heartbeatAccountHidden()
{
    const auto *settings = getSettings();
    return !settings->sendActivityHeartbeats ||
           settings->hideAccountInHeartbeats;
}

QString heartbeatMode()
{
    const auto *settings = getSettings();
    if (!settings->sendActivityHeartbeats)
    {
        return QStringLiteral("disabled");
    }
    if (settings->hideAccountInHeartbeats)
    {
        return QStringLiteral("anonymous");
    }
    return QStringLiteral("normal");
}

QString formatBytes(qint64 bytes)
{
    if (bytes >= 1024 * 1024)
    {
        return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + " MB";
    }

    if (bytes >= 1024)
    {
        return QString::number(bytes / 1024.0, 'f', 1) + " KB";
    }

    return QString::number(bytes) + " B";
}

#ifdef Q_OS_WIN

QString quotePowerShell(QString value)
{
    value.replace('\'', "''");
    return QString("'%1'").arg(value);
}

QString quoteWindowsArg(const QString &value)
{
    if (value.isEmpty())
    {
        return "\"\"";
    }

    bool needsQuotes = false;
    for (const auto ch : value)
    {
        if (ch.isSpace() || ch == '"')
        {
            needsQuotes = true;
            break;
        }
    }

    if (!needsQuotes)
    {
        return value;
    }

    QString quoted = "\"";
    int backslashes = 0;

    for (const auto ch : value)
    {
        if (ch == '\\')
        {
            ++backslashes;
            continue;
        }

        if (ch == '"')
        {
            quoted += QString(backslashes * 2 + 1, '\\');
            quoted += '"';
            backslashes = 0;
            continue;
        }

        if (backslashes > 0)
        {
            quoted += QString(backslashes, '\\');
            backslashes = 0;
        }

        quoted += ch;
    }

    if (backslashes > 0)
    {
        quoted += QString(backslashes * 2, '\\');
    }

    quoted += '"';
    return quoted;
}

QString joinWindowsArgs(const QStringList &args)
{
    QStringList quoted;
    quoted.reserve(args.size());

    for (const auto &arg : args)
    {
        quoted << quoteWindowsArg(arg);
    }

    return quoted.join(' ');
}

QString powershellPath()
{
    auto systemRoot = qEnvironmentVariable("SystemRoot");
    if (systemRoot.isEmpty())
    {
        systemRoot = "C:/Windows";
    }

    auto path = QDir(systemRoot)
                    .filePath("System32/WindowsPowerShell/v1.0/powershell.exe");
    if (QFileInfo::exists(path))
    {
        return QDir::toNativeSeparators(path);
    }

    return "powershell.exe";
}

bool launchAfterThisProcessExits(const QString &installerPath,
                                 const QStringList &installerArgs,
                                 const QString &workingDirectory)
{
    const auto pid = QCoreApplication::applicationPid();
    const auto script =
        QString(
            "$ErrorActionPreference = 'Stop'\r\n"
            "$process = Get-Process -Id %1 -ErrorAction SilentlyContinue\r\n"
            "if ($null -ne $process) { Wait-Process -Id %1 }\r\n"
            "$startInfo = New-Object System.Diagnostics.ProcessStartInfo\r\n"
            "$startInfo.FileName = %2\r\n"
            "$startInfo.Arguments = %3\r\n"
            "$startInfo.WorkingDirectory = %4\r\n"
            "$startInfo.UseShellExecute = $true\r\n"
            "[System.Diagnostics.Process]::Start($startInfo) | Out-Null\r\n")
            .arg(QString::number(pid),
                 quotePowerShell(QDir::toNativeSeparators(installerPath)),
                 quotePowerShell(joinWindowsArgs(installerArgs)),
                 quotePowerShell(QDir::toNativeSeparators(workingDirectory)));

    const QByteArray scriptBytes(reinterpret_cast<const char *>(script.utf16()),
                                 script.size() * 2);

    return QProcess::startDetached(
        powershellPath(),
        {"-NoProfile", "-ExecutionPolicy", "Bypass", "-WindowStyle", "Hidden",
         "-EncodedCommand", QString::fromLatin1(scriptBytes.toBase64())},
        workingDirectory);
}

void quitForInstaller()
{
    QTimer::singleShot(0, QCoreApplication::instance(), [] {
        if (auto *app = tryGetApp())
        {
            if (auto *windows = app->getWindows())
            {
                windows->save();
                windows->closeAll();
            }
        }

        QCoreApplication::quit();
    });
}

#endif

}  // namespace

class MoltorinoBadgeSocketListener : public WebSocketListener
{
public:
    MoltorinoBadgeSocketListener(MoltorinoPresence *presence, int generation)
        : presence_(presence)
        , generation_(generation)
    {
    }

    void onOpen() override
    {
        runInGuiThread(
            [presence = this->presence_, generation = this->generation_] {
                if (presence != nullptr)
                {
                    presence->handleBadgeSocketOpen(generation);
                }
            });
    }

    void onTextMessage(QByteArray data) override
    {
        runInGuiThread([presence = this->presence_,
                        generation = this->generation_,
                        data = std::move(data)]() mutable {
            if (presence != nullptr)
            {
                presence->handleBadgeSocketMessage(generation, std::move(data));
            }
        });
    }

    void onBinaryMessage(QByteArray data) override
    {
        this->onTextMessage(std::move(data));
    }

    void onClose(std::unique_ptr<WebSocketListener>) override
    {
        runInGuiThread(
            [presence = this->presence_, generation = this->generation_] {
                if (presence != nullptr)
                {
                    presence->handleBadgeSocketClosed(generation);
                }
            });
    }

private:
    MoltorinoPresence *presence_{};
    int generation_{};
};

MoltorinoPresence::MoltorinoPresence()
//    : clientInstanceId_(savedClientId())
{
    // this->heartbeatTimer_.setInterval(HEARTBEAT_INTERVAL_MS);
    // this->heartbeatTimer_.setTimerType(Qt::VeryCoarseTimer);

    // QObject::connect(&this->heartbeatTimer_, &QTimer::timeout, this, [this] {
    //     this->sendHeartbeat();
    // });

    // this->badgeSocketReconnectTimer_.setSingleShot(true);
    // this->badgeSocketReconnectTimer_.setTimerType(Qt::VeryCoarseTimer);
    // QObject::connect(&this->badgeSocketReconnectTimer_, &QTimer::timeout, this,
    //                  [this] {
    //                      this->connectBadgeSocket(true);
    //                  });
}

MoltorinoPresence &MoltorinoPresence::instance()
{
    static MoltorinoPresence presence;
    return presence;
}

void MoltorinoPresence::init()
{
    if (this->initialized_)
    {
        return;
    }

    this->initialized_ = true;
    this->accountChangedConnection_ =
        getApp()->getAccounts()->twitch.currentUserChanged.connect([this] {
            this->sendHeartbeat(true);
        });
    this->transmitPresenceConnection_ =
        getSettings()->transmitPresence.connect([this](bool) {
            this->applyHeartbeatSettings(true);
        }, false);
    this->activityHeartbeatConnection_ =
        getSettings()->sendActivityHeartbeats.connect([this](bool) {
            this->applyHeartbeatSettings(true);
        }, false);
    this->heartbeatAccountConnection_ =
        getSettings()->hideAccountInHeartbeats.connect([this](bool) {
            this->applyHeartbeatSettings(true);
        }, false);

    this->connectBadgeSocket();
}

void MoltorinoPresence::startHeartbeat()
{
    this->connectBadgeSocket();
    this->applyHeartbeatSettings(true);
}

void MoltorinoPresence::applyHeartbeatSettings(bool sendNow)
{
    if (!activityHeartbeatsEnabled())
    {
        this->heartbeatTimer_.stop();
        this->heartbeatQueued_ = false;
        return;
    }

    if (!this->heartbeatTimer_.isActive())
    {
        this->heartbeatTimer_.start();
    }

    if (sendNow)
    {
        this->sendHeartbeat(true);
    }
}

bool MoltorinoPresence::shouldShowUpdateButton() const
{
    return this->availableUpdate_.has_value();
}

bool MoltorinoPresence::isUpdateError() const
{
    return this->updateError_;
}

bool MoltorinoPresence::isUpdateBusy() const
{
#ifdef Q_OS_WIN
    return this->updateDownloadInFlight_ || this->updateLaunching_;
#else
    return false;
#endif
}

void MoltorinoPresence::installAvailableUpdate()
{
    if (!this->availableUpdate_)
    {
        return;
    }

#ifdef Q_OS_WIN
    if (this->updateReady_)
    {
        this->showInstallPrompt(true);
        return;
    }

    if (!this->updateDownloadInFlight_ && !this->updateLaunching_)
    {
        this->beginInstallerDownload(true);
    }
#else
    this->showDownloadPrompt(true);
#endif
}

void MoltorinoPresence::sendHeartbeat(bool force)
{
    if (!activityHeartbeatsEnabled())
    {
        this->heartbeatTimer_.stop();
        this->heartbeatQueued_ = false;
        return;
    }

    if (this->heartbeatInFlight_)
    {
        this->heartbeatQueued_ = this->heartbeatQueued_ || force;
        return;
    }

    this->heartbeatInFlight_ = true;
    this->heartbeatQueued_ = false;

    NetworkRequest(apiUrl(HEARTBEAT_PATH), NetworkRequestType::Post)
        .timeout(15000)
        .json(this->makePayload())
        .onSuccess([this](const NetworkResult &result) {
            this->heartbeatInFlight_ = false;
            if (activityHeartbeatsEnabled())
            {
                this->handleServerReply(result.parseJson());
            }

            if (this->heartbeatQueued_)
            {
                this->sendHeartbeat(true);
            }
        })
        .onError([this](const NetworkResult &result) {
            this->heartbeatInFlight_ = false;
            qCWarning(chatterinoMoltorinoPresence)
                << "presence heartbeat failed:" << result.formatError();

            if (this->heartbeatQueued_)
            {
                this->sendHeartbeat(true);
            }
        })
        .execute();
}

void MoltorinoPresence::handleServerReply(const QJsonObject &root)
{
    const auto updateChanged = this->setAvailableUpdate(
        this->parseUpdate(root.value(QStringLiteral("update")).toObject()));

    if (updateChanged && this->availableUpdate_)
    {
        this->showUpdatePrompt(false);
    }
}

bool MoltorinoPresence::setAvailableUpdate(
    std::optional<MoltorinoUpdateInfo> update)
{
    const auto oldFingerprint =
        this->availableUpdate_
            ? this->updateFingerprint(*this->availableUpdate_)
            : QString();
    const auto newFingerprint =
        update ? this->updateFingerprint(*update) : QString();

    if (oldFingerprint == newFingerprint)
    {
        this->availableUpdate_ = std::move(update);
        return false;
    }

#ifdef Q_OS_WIN
    this->cleanupInstallerDownload(true);
    this->updateDownloadInFlight_ = false;
    this->updateReady_ = false;
    this->updateLaunching_ = false;
#endif

    this->availableUpdate_ = std::move(update);
    this->updatePromptDismissedThisRun_ = false;
    this->updateError_ = false;
    this->updateStateChanged.invoke();
    return true;
}

void MoltorinoPresence::connectBadgeSocket(bool force)
{
    Q_UNUSED(force);

    // if (!badgeSocketConfigured())
    // {
    //     return;
    // }

    // if (!force && (this->badgeSocketConnecting_ || this->badgeSocketOpen_))
    // {
    //     return;
    // }

    // if (force && (this->badgeSocketConnecting_ || this->badgeSocketOpen_))
    // {
    //     this->disconnectBadgeSocket();
    // }

    // if (!this->badgeSocketPool_)
    // {
    //     this->badgeSocketPool_ =
    //         std::make_unique<WebSocketPool>(QStringLiteral("Leafyrino badges"));
    // }

    // this->badgeSocketReconnectTimer_.stop();
    // this->badgeSocketClosing_ = false;
    // this->badgeSocketConnecting_ = true;
    // this->badgeSocketOpen_ = false;

    // const auto generation = ++this->badgeSocketGeneration_;
    // this->badgeSocket_ = this->badgeSocketPool_->createSocket(
    //     WebSocketOptions{
    //         .url = badgeSocketUrl(),
    //         .headers = {},
    //     },
    //     std::make_unique<MoltorinoBadgeSocketListener>(this, generation));
}

void MoltorinoPresence::disconnectBadgeSocket()
{
    this->badgeSocketReconnectTimer_.stop();
    this->badgeSocketClosing_ = true;
    ++this->badgeSocketGeneration_;

    this->badgeSocket_.close();
    this->badgeSocket_ = WebSocketHandle();
    this->badgeSocketConnecting_ = false;
    this->badgeSocketOpen_ = false;
}

void MoltorinoPresence::handleBadgeSocketOpen(int generation)
{
    if (generation != this->badgeSocketGeneration_)
    {
        return;
    }

    this->badgeSocketConnecting_ = false;
    this->badgeSocketOpen_ = true;
    this->badgeSocketBackoffStep_ = 0;
}

void MoltorinoPresence::handleBadgeSocketClosed(int generation)
{
    if (generation != this->badgeSocketGeneration_)
    {
        return;
    }

    this->badgeSocketConnecting_ = false;
    this->badgeSocketOpen_ = false;

    if (this->badgeSocketClosing_ || !badgeSocketConfigured())
    {
        this->badgeSocketClosing_ = false;
        return;
    }

    const auto shift = std::min(this->badgeSocketBackoffStep_, 4);
    const auto baseDelay =
        std::min(BADGE_SOCKET_RECONNECT_MAX_MS,
                 BADGE_SOCKET_RECONNECT_BASE_MS * (1 << shift));
    const auto jitter = int(QRandomGenerator::global()->bounded(5000));

    this->badgeSocketBackoffStep_ =
        std::min(this->badgeSocketBackoffStep_ + 1, 8);
    this->badgeSocketReconnectTimer_.start(baseDelay + jitter);
}

void MoltorinoPresence::handleBadgeSocketMessage(int generation,
                                                 QByteArray data)
{
    if (generation != this->badgeSocketGeneration_)
    {
        return;
    }

    const auto doc = QJsonDocument::fromJson(data);
    if (!doc.isObject())
    {
        return;
    }

    const auto root = doc.object();
    const auto type =
        root.value(QStringLiteral("type")).toString().trimmed().toLower();
    if (type != QStringLiteral("badge_update") &&
        type != QStringLiteral("badges_updated") &&
        type != QStringLiteral("moltorino_badges_updated"))
    {
        return;
    }

    auto version = root.value(QStringLiteral("version")).toInt(-1);
    if (version < 0)
    {
        version = root.value(QStringLiteral("data"))
                      .toObject()
                      .value(QStringLiteral("version"))
                      .toInt(-1);
    }

    if (auto *badges = getApp()->getMoltorinoSupporterBadges())
    {
        if (version >= 0)
        {
            badges->refreshIfNewer(version);
        }
        else
        {
            badges->refreshNow();
        }
    }
}

QJsonObject MoltorinoPresence::makePayload() const
{
    const auto now = QDateTime::currentDateTimeUtc();

    QJsonObject payload;
    payload.insert(QStringLiteral("clientInstanceId"), this->clientInstanceId_);
    payload.insert(QStringLiteral("platform"), platformKey());
    payload.insert(QStringLiteral("appVersion"), Version::instance().version());
    payload.insert(QStringLiteral("sentAt"), now.toString(Qt::ISODate));
    payload.insert(QStringLiteral("heartbeatMode"), heartbeatMode());
    payload.insert(QStringLiteral("status"),
                   QGuiApplication::applicationState() == Qt::ApplicationActive
                       ? QStringLiteral("active")
                       : QStringLiteral("background"));

    if (!heartbeatAccountHidden())
    {
        payload.insert(QStringLiteral("activeAccount"), this->activeAccount());
    }

    return payload;
}

QJsonObject MoltorinoPresence::activeAccount() const
{
    const auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!account || account->isAnon())
    {
        return {};
    }

    QJsonObject root;
    root.insert(QStringLiteral("userId"), account->getUserId());
    root.insert(QStringLiteral("username"), account->getUserName());
    return root;
}

std::optional<MoltorinoUpdateInfo> MoltorinoPresence::parseUpdate(
    const QJsonObject &root) const
{
    if (root.isEmpty())
    {
        return std::nullopt;
    }

    MoltorinoUpdateInfo update;
    update.id = root.value(QStringLiteral("id")).toString();
    update.version = root.value(QStringLiteral("version")).toString();
    update.downloadUrl = root.value(QStringLiteral("downloadUrl")).toString();
    update.sha256 = root.value(QStringLiteral("sha256")).toString();
    update.developerNote =
        root.value(QStringLiteral("developerNote")).toString();
    update.sizeBytes =
        root.value(QStringLiteral("sizeBytes")).toVariant().toLongLong();

    const QUrl downloadUrl(update.downloadUrl.trimmed());
    const auto scheme = downloadUrl.scheme().toLower();
    if (!downloadUrl.isValid() || (scheme != "https" && scheme != "http"))
    {
        return std::nullopt;
    }

    return update;
}

QString MoltorinoPresence::updateFingerprint(
    const MoltorinoUpdateInfo &update) const
{
    return QString("%1\n%2\n%3")
        .arg(update.id, update.sha256.trimmed().toLower(),
             update.downloadUrl.trimmed());
}

bool MoltorinoPresence::showUpdatePrompt(bool force)
{
#ifdef Q_OS_WIN
    if (this->updateReady_)
    {
        return this->showInstallPrompt(force);
    }
#endif

    return this->showDownloadPrompt(force);
}

bool MoltorinoPresence::showDownloadPrompt(bool force)
{
    if (!this->availableUpdate_)
    {
        return false;
    }

    if (force)
    {
        this->updatePromptDismissedThisRun_ = false;
    }

    if (!force && this->updatePromptDismissedThisRun_)
    {
        return false;
    }

    if (this->updatePrompt_)
    {
        this->updatePrompt_->raise();
        this->updatePrompt_->activateWindow();
        return true;
    }

    const auto update = *this->availableUpdate_;
    QString text = QStringLiteral("<p><b>Leafyrino %1 is available.</b></p>")
                       .arg(update.version.toHtmlEscaped());

    if (update.sizeBytes > 0)
    {
        text += QStringLiteral("<p>Download size: %1</p>")
                    .arg(formatBytes(update.sizeBytes).toHtmlEscaped());
    }

    if (!update.developerNote.isEmpty())
    {
        auto note = update.developerNote.toHtmlEscaped();
        note.replace('\n', "<br>");
        text += QStringLiteral("<p>%1</p>").arg(note);
    }

#ifdef Q_OS_WIN
    const auto actionText = QStringLiteral("Download");
#elif defined(Q_OS_MACOS)
    const auto actionText = QStringLiteral("Open download");
    text += QStringLiteral(
        "<p>Download the DMG, quit Leafyrino, then replace the app.</p>");
#elif defined(Q_OS_LINUX)
    const auto actionText = QStringLiteral("Open download");
    text += QStringLiteral(
        "<p>Download the AppImage, quit Leafyrino, then run the new file.</p>");
#else
    const auto actionText = QStringLiteral("Open download");
#endif

    auto *box = new QMessageBox(QMessageBox::Information,
                                QStringLiteral("Leafyrino update"), text,
                                QMessageBox::NoButton, this->dialogParent());
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    box->setWindowModality(Qt::NonModal);
    box->setTextFormat(Qt::RichText);

    auto *downloadButton = box->addButton(actionText, QMessageBox::AcceptRole);
    auto *laterButton =
        box->addButton(QStringLiteral("Not now"), QMessageBox::RejectRole);
    box->setDefaultButton(downloadButton);

    QObject::connect(box, &QMessageBox::finished, this,
                     [this, box, laterButton] {
                         if (box->clickedButton() == laterButton ||
                             box->clickedButton() == nullptr)
                         {
                             this->updatePromptDismissedThisRun_ = true;
                         }

                         this->updatePrompt_.clear();
                     });

    QObject::connect(
        box, &QMessageBox::buttonClicked, this,
        [this, box, downloadButton, update](QAbstractButton *button) {
            if (button != downloadButton)
            {
                return;
            }

#ifdef Q_OS_WIN
            box->accept();
            this->beginInstallerDownload(true);
#else
            if (this->openDownloadPage(update))
            {
                box->accept();
            }
#endif
        });

    this->updatePrompt_ = box;
    box->show();
    box->raise();
    box->activateWindow();
    return true;
}

bool MoltorinoPresence::showInstallPrompt(bool force)
{
#ifndef Q_OS_WIN
    (void)force;
    return false;
#else
    if (!this->availableUpdate_ || !this->updateReady_)
    {
        return false;
    }

    if (force)
    {
        this->updatePromptDismissedThisRun_ = false;
    }

    if (!force && this->updatePromptDismissedThisRun_)
    {
        return false;
    }

    if (this->updatePrompt_)
    {
        this->updatePrompt_->raise();
        this->updatePrompt_->activateWindow();
        return true;
    }

    const auto update = *this->availableUpdate_;
    auto text =
        QStringLiteral(
            "<p><b>Leafyrino %1 is downloaded and ready.</b></p>"
            "<p>Restart Leafyrino when you are ready to install it.</p>")
            .arg(update.version.toHtmlEscaped());

    auto *box = new QMessageBox(QMessageBox::Information,
                                QStringLiteral("Leafyrino update ready"), text,
                                QMessageBox::NoButton, this->dialogParent());
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    box->setWindowModality(Qt::NonModal);
    box->setTextFormat(Qt::RichText);

    auto *installButton = box->addButton(QStringLiteral("Restart and install"),
                                         QMessageBox::AcceptRole);
    auto *laterButton =
        box->addButton(QStringLiteral("Not now"), QMessageBox::RejectRole);
    box->setDefaultButton(installButton);

    QObject::connect(box, &QMessageBox::finished, this,
                     [this, box, laterButton] {
                         if (box->clickedButton() == laterButton ||
                             box->clickedButton() == nullptr)
                         {
                             this->updatePromptDismissedThisRun_ = true;
                         }

                         this->updatePrompt_.clear();
                     });

    QObject::connect(box, &QMessageBox::buttonClicked, this,
                     [this, box, installButton](QAbstractButton *button) {
                         if (button == installButton)
                         {
                             box->hide();
                             this->launchDownloadedInstaller();
                         }
                     });

    this->updatePrompt_ = box;
    box->show();
    box->raise();
    box->activateWindow();
    return true;
#endif
}

bool MoltorinoPresence::openDownloadPage(const MoltorinoUpdateInfo &update)
{
    this->updateError_ = false;
    const auto ok = QDesktopServices::openUrl(QUrl(update.downloadUrl));

    if (!ok)
    {
        this->updateError_ = true;
        this->updateStateChanged.invoke();
    }

    return ok;
}

QWidget *MoltorinoPresence::dialogParent() const
{
    if (QApplication::activeWindow())
    {
        return QApplication::activeWindow();
    }

    return &getApp()->getWindows()->getMainWindow();
}

#ifdef Q_OS_WIN

void MoltorinoPresence::beginInstallerDownload(bool showProgress)
{
    if (!this->availableUpdate_ || this->updateDownloadInFlight_ ||
        this->updateLaunching_)
    {
        return;
    }

    const auto update = *this->availableUpdate_;
    if (this->existingInstallerIsReady(update))
    {
        this->showInstallPrompt(true);
        return;
    }

    this->cleanupInstallerDownload(true);

    QString fileError;
    if (!this->openInstallerFile(fileError))
    {
        this->finishInstallerDownload(false, fileError);
        return;
    }

    const QUrl url(update.downloadUrl);
    const auto scheme = url.scheme().toLower();
    if (!url.isValid() || (scheme != "https" && scheme != "http"))
    {
        this->finishInstallerDownload(
            false, "The update link from the server was not valid.");
        return;
    }

    this->updateDownloadInFlight_ = true;
    this->updateReady_ = false;
    this->updateError_ = false;
    this->installerHash_.clear();
    this->installerDownloadError_.clear();
    this->installerDownloadCanceled_ = false;
    this->updateStateChanged.invoke();

    auto *progress = static_cast<QProgressDialog *>(nullptr);
    if (showProgress)
    {
        progress = new QProgressDialog(
            QStringLiteral("Downloading Leafyrino update..."),
            QStringLiteral("Cancel"), 0, 100, this->dialogParent());
        progress->setAttribute(Qt::WA_DeleteOnClose);
        progress->setModal(false);
        progress->setWindowModality(Qt::NonModal);
        progress->setMinimumDuration(150);
        progress->show();
        this->updateProgress_ = progress;
    }

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(UPDATE_DOWNLOAD_TIMEOUT_MS);

    auto *reply = this->updateNetwork_.get(request);
    this->updateReply_ = reply;
    auto hash =
        std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);

    if (progress)
    {
        QObject::connect(progress, &QProgressDialog::canceled, this, [this] {
            this->cancelInstallerDownload();
        });
    }

    QObject::connect(
        reply, &QNetworkReply::downloadProgress, this,
        [this, reply](qint64 received, qint64 total) {
            if (this->updateReply_ != reply || !this->updateProgress_)
            {
                return;
            }

            if (total > 0)
            {
                this->updateProgress_->setMaximum(100);
                this->updateProgress_->setValue(
                    static_cast<int>((received * 100) / total));
                this->updateProgress_->setLabelText(
                    QStringLiteral("Downloading update... %1 / %2")
                        .arg(formatBytes(received), formatBytes(total)));
            }
            else
            {
                this->updateProgress_->setMaximum(0);
            }
        });

    QObject::connect(reply, &QIODevice::readyRead, this, [this, reply, hash] {
        if (this->updateReply_ != reply)
        {
            return;
        }

        const auto chunk = reply->readAll();
        if (chunk.isEmpty())
        {
            return;
        }

        hash->addData(chunk);
        if (this->installerFile_.write(chunk) != chunk.size())
        {
            this->installerDownloadError_ =
                QStringLiteral("Could not write the installer to disk.");
            reply->abort();
        }
    });

    QObject::connect(
        reply, &QNetworkReply::finished, this, [this, reply, hash] {
            if (this->updateReply_ != reply)
            {
                reply->deleteLater();
                return;
            }

            const auto tail = reply->readAll();
            if (!tail.isEmpty())
            {
                hash->addData(tail);
                if (this->installerFile_.write(tail) != tail.size())
                {
                    this->installerDownloadError_ = QStringLiteral(
                        "Could not write the installer to disk.");
                    this->finishInstallerDownload(
                        false, this->installerDownloadError_);
                    return;
                }
            }

            this->installerFile_.flush();
            this->installerFile_.close();
            this->installerHash_ = hash->result().toHex();

            const auto status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                    .toInt();
            const auto failed = reply->error() != QNetworkReply::NoError ||
                                status < 200 || status >= 300;
            const auto errorText = reply->errorString();
            const auto quietCancel =
                this->installerDownloadCanceled_ &&
                this->installerDownloadError_.isEmpty() &&
                reply->error() == QNetworkReply::OperationCanceledError;

            this->updateReply_.clear();
            reply->deleteLater();

            if (failed)
            {
                if (quietCancel)
                {
                    this->updateDownloadInFlight_ = false;
                    this->updateReady_ = false;
                    this->updateError_ = false;
                    this->installerDownloadCanceled_ = false;
                    this->cleanupInstallerDownload(true);
                    this->updateStateChanged.invoke();
                    return;
                }

                this->finishInstallerDownload(
                    false,
                    !this->installerDownloadError_.isEmpty()
                        ? this->installerDownloadError_
                        : (errorText.isEmpty()
                               ? QStringLiteral("Installer download failed.")
                               : errorText));
                return;
            }

            const auto expected =
                this->availableUpdate_->sha256.trimmed().toLower();
            const auto actual =
                QString::fromLatin1(this->installerHash_).toLower();
            if (!expected.isEmpty() && expected != actual)
            {
                this->finishInstallerDownload(
                    false, QStringLiteral("The downloaded installer did not "
                                          "match the update checksum."));
                return;
            }

            this->finishInstallerDownload(true);
        });
}

void MoltorinoPresence::finishInstallerDownload(bool success,
                                                const QString &error)
{
    if (this->updateProgress_)
    {
        this->updateProgress_->close();
        this->updateProgress_.clear();
    }

    if (this->updateReply_)
    {
        auto *reply = this->updateReply_.data();
        this->updateReply_.clear();
        if (reply->isRunning())
        {
            reply->abort();
        }
        reply->deleteLater();
    }

    if (this->installerFile_.isOpen())
    {
        this->installerFile_.close();
    }

    this->updateDownloadInFlight_ = false;
    this->installerDownloadCanceled_ = false;

    if (!success)
    {
        this->updateReady_ = false;
        this->updateError_ = true;
        this->installerDownloadError_.clear();
        this->cleanupInstallerDownload(true);
        this->updateStateChanged.invoke();

        auto *box = new QMessageBox(
            QMessageBox::Warning, QStringLiteral("Leafyrino update"),
            error.isEmpty() ? QStringLiteral("Update failed.") : error,
            QMessageBox::Ok, this->dialogParent());
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->show();
        return;
    }

    this->updateError_ = false;
    this->updateReady_ = true;
    this->installerDownloadError_.clear();
    this->updatePromptDismissedThisRun_ = false;
    this->updateStateChanged.invoke();
    this->showInstallPrompt(false);
}

void MoltorinoPresence::launchDownloadedInstaller()
{
    if (!this->availableUpdate_ || this->installerPath_.isEmpty() ||
        !QFileInfo::exists(this->installerPath_))
    {
        this->updateReady_ = false;
        this->updateError_ = true;
        this->updateStateChanged.invoke();
        this->beginInstallerDownload(true);
        return;
    }

    const auto expected = this->availableUpdate_->sha256.trimmed().toLower();
    if (!expected.isEmpty())
    {
        QString hashError;
        const auto actual =
            QString::fromLatin1(this->hashFile(this->installerPath_, hashError))
                .toLower();
        if (!hashError.isEmpty() || actual != expected)
        {
            this->cleanupInstallerDownload(true);
            this->updateReady_ = false;
            this->updateError_ = true;
            this->updateStateChanged.invoke();
            this->beginInstallerDownload(true);
            return;
        }
    }

    this->updateLaunching_ = true;
    this->updateReady_ = false;
    this->updateError_ = false;
    this->updateStateChanged.invoke();

    const auto installerDir = QFileInfo(this->installerPath_).absoluteDir();
    const auto logPath = installerDir.filePath("Leafyrino7-update-install.log");
    if (!launchAfterThisProcessExits(this->installerPath_,
                                     this->installerArgs(logPath),
                                     installerDir.absolutePath()))
    {
        this->updateLaunching_ = false;
        this->updateReady_ = true;
        this->updateError_ = true;
        this->updateStateChanged.invoke();
        return;
    }

    quitForInstaller();
}

QString MoltorinoPresence::installerFileName() const
{
    return QStringLiteral("Leafyrino Updater.exe");
}

QStringList MoltorinoPresence::installerArgs(const QString &logPath) const
{
    QStringList args{
        QStringLiteral("/VERYSILENT"),
        QStringLiteral("/SUPPRESSMSGBOXES"),
        QStringLiteral("/NORESTART"),
        QStringLiteral("/SP-"),
        QStringLiteral("/CLOSEAPPLICATIONS"),
        QStringLiteral("/MERGETASKS=!freshinstall,!desktopicon"),
    };

    if (!logPath.isEmpty())
    {
        args << QStringLiteral("/LOG=%1").arg(
            QDir::toNativeSeparators(logPath));
    }

    return args;
}

QString MoltorinoPresence::archiveToolPath() const
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("support/7zip/7z.exe"));
}

bool MoltorinoPresence::extractInstallerArchive(const QString &archivePath,
                                                const QString &targetDirectory,
                                                QString &error) const
{
    const auto toolPath = this->archiveToolPath();
    if (!QFileInfo::exists(toolPath))
    {
        error = QStringLiteral("Could not find the bundled archive tool.");
        return false;
    }

    if (!QFileInfo::exists(archivePath))
    {
        error = QStringLiteral("Installer archive was not found.");
        return false;
    }

    QDir target(targetDirectory);
    if (!target.mkpath(QStringLiteral(".")))
    {
        error = QStringLiteral("Could not create the extraction folder.");
        return false;
    }

    QProcess process;
    process.setProgram(toolPath);
    process.setArguments({
        QStringLiteral("x"),
        QStringLiteral("-y"),
        QStringLiteral("-aoa"),
        QStringLiteral("-o%1").arg(
            QDir::toNativeSeparators(target.absolutePath())),
        QDir::toNativeSeparators(archivePath),
    });
    process.setWorkingDirectory(target.absolutePath());
    process.start();

    if (!process.waitForStarted(5000))
    {
        error = process.errorString();
        return false;
    }

    if (!process.waitForFinished(UPDATE_DOWNLOAD_TIMEOUT_MS))
    {
        process.kill();
        process.waitForFinished(1000);
        error = QStringLiteral("Installer archive extraction timed out.");
        return false;
    }

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    {
        auto details =
            QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        if (details.isEmpty())
        {
            details = QString::fromLocal8Bit(process.readAllStandardOutput())
                          .trimmed();
        }

        error = details.isEmpty()
                    ? QStringLiteral("Installer archive extraction failed.")
                    : details;
        return false;
    }

    return true;
}

QStringList MoltorinoPresence::downloadFolders() const
{
    QStringList folders;
    folders << getApp()->getPaths().miscDirectory + "/Updates";

    const auto temp =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (!temp.isEmpty())
    {
        folders << QDir(temp).filePath("Leafyrino/Updates");
    }

    return folders;
}

bool MoltorinoPresence::openInstallerFile(QString &error)
{
    QStringList tried;

    for (const auto &folder : this->downloadFolders())
    {
        QDir dir(folder);
        if (!dir.mkpath("."))
        {
            tried << QDir::toNativeSeparators(folder) + " (could not create)";
            continue;
        }

        const auto path = dir.filePath(this->installerFileName());
        QFile::remove(path);

        this->installerFile_.setFileName(path);
        if (this->installerFile_.open(QIODevice::WriteOnly |
                                      QIODevice::Truncate))
        {
            this->installerPath_ = path;
            return true;
        }

        tried << QStringLiteral("%1 (%2)").arg(
            QDir::toNativeSeparators(path), this->installerFile_.errorString());
    }

    error = QStringLiteral("Could not prepare the installer download.");
    if (!tried.isEmpty())
    {
        error += QStringLiteral("\n\nTried:\n") + tried.join('\n');
    }

    return false;
}

bool MoltorinoPresence::existingInstallerIsReady(
    const MoltorinoUpdateInfo &update)
{
    const auto expected = update.sha256.trimmed().toLower();
    if (expected.isEmpty())
    {
        return false;
    }

    for (const auto &folder : this->downloadFolders())
    {
        const auto path = QDir(folder).filePath(this->installerFileName());
        if (!QFileInfo::exists(path))
        {
            continue;
        }

        QString error;
        const auto actual =
            QString::fromLatin1(this->hashFile(path, error)).toLower();
        if (!error.isEmpty() || actual != expected)
        {
            QFile::remove(path);
            continue;
        }

        this->installerPath_ = path;
        this->installerHash_ = actual.toLatin1();
        this->updateReady_ = true;
        this->updateError_ = false;
        this->updateStateChanged.invoke();
        return true;
    }

    return false;
}

void MoltorinoPresence::cleanupInstallerDownload(bool removeFile)
{
    if (this->updateProgress_)
    {
        this->updateProgress_->close();
        this->updateProgress_.clear();
    }

    if (this->updateReply_)
    {
        auto *reply = this->updateReply_.data();
        this->updateReply_.clear();
        if (reply->isRunning())
        {
            reply->abort();
        }
        reply->deleteLater();
    }

    if (this->installerFile_.isOpen())
    {
        this->installerFile_.close();
    }

    if (removeFile && !this->installerPath_.isEmpty())
    {
        QFile::remove(this->installerPath_);
        this->installerPath_.clear();
        this->installerHash_.clear();
    }
}

void MoltorinoPresence::cancelInstallerDownload()
{
    this->installerDownloadCanceled_ = true;

    if (this->updateReply_)
    {
        this->updateReply_->abort();
    }
}

QByteArray MoltorinoPresence::hashFile(const QString &path,
                                       QString &error) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        error = file.errorString();
        return {};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd())
    {
        hash.addData(file.read(1024 * 1024));
    }

    return hash.result().toHex();
}

#endif

MoltorinoPresence *getMoltorinoPresence()
{
    return &MoltorinoPresence::instance();
}

}  // namespace chatterino

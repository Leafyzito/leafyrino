#pragma once

#include "common/websockets/WebSocketPool.hpp"

#include <boost/signals2/connection.hpp>
#include <pajlada/signals/signal.hpp>
#include <QByteArray>
#include <QFile>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>
#include <optional>

class QMessageBox;
class QNetworkReply;
class QProgressDialog;
class QWidget;

namespace chatterino {

class MoltorinoBadgeSocketListener;

struct MoltorinoUpdateInfo {
    QString id;
    QString version;
    QString downloadUrl;
    QString sha256;
    QString developerNote;
    qint64 sizeBytes{};
};

class MoltorinoPresence : public QObject
{
public:
    MoltorinoPresence();
    static MoltorinoPresence &instance();

    void init();
    void startHeartbeat();

    bool shouldShowUpdateButton() const;
    bool isUpdateError() const;
    bool isUpdateBusy() const;
    void installAvailableUpdate();

    pajlada::Signals::NoArgSignal updateStateChanged;

private:
    void applyHeartbeatSettings(bool sendNow = false);
    void sendHeartbeat(bool force = false);
    void handleServerReply(const QJsonObject &root);
    bool setAvailableUpdate(std::optional<MoltorinoUpdateInfo> update);
    void connectBadgeSocket(bool force = false);
    void disconnectBadgeSocket();
    void handleBadgeSocketOpen(int generation);
    void handleBadgeSocketClosed(int generation);
    void handleBadgeSocketMessage(int generation, QByteArray data);

    QJsonObject makePayload() const;
    QJsonObject activeAccount() const;
    std::optional<MoltorinoUpdateInfo> parseUpdate(
        const QJsonObject &root) const;
    QString updateFingerprint(const MoltorinoUpdateInfo &update) const;

    bool showUpdatePrompt(bool force = false);
    bool showDownloadPrompt(bool force = false);
    bool showInstallPrompt(bool force = false);
    bool openDownloadPage(const MoltorinoUpdateInfo &update);
    QWidget *dialogParent() const;

#ifdef Q_OS_WIN
    void beginInstallerDownload(bool showProgress);
    void finishInstallerDownload(bool success, const QString &error = {});
    void launchDownloadedInstaller();
    QString installerFileName() const;
    QStringList installerArgs(const QString &logPath) const;
    QString archiveToolPath() const;
    bool extractInstallerArchive(const QString &archivePath,
                                 const QString &targetDirectory,
                                 QString &error) const;
    QStringList downloadFolders() const;
    bool openInstallerFile(QString &error);
    bool existingInstallerIsReady(const MoltorinoUpdateInfo &update);
    void cleanupInstallerDownload(bool removeFile);
    void cancelInstallerDownload();
    QByteArray hashFile(const QString &path, QString &error) const;
#endif

    QTimer heartbeatTimer_;
    QTimer badgeSocketReconnectTimer_;
    boost::signals2::scoped_connection accountChangedConnection_;
    boost::signals2::scoped_connection transmitPresenceConnection_;
    boost::signals2::scoped_connection activityHeartbeatConnection_;
    boost::signals2::scoped_connection heartbeatAccountConnection_;

    std::unique_ptr<WebSocketPool> badgeSocketPool_;
    WebSocketHandle badgeSocket_;

    bool initialized_{};
    bool heartbeatInFlight_{};
    bool heartbeatQueued_{};
    bool updatePromptDismissedThisRun_{};
    bool updateError_{};
    bool badgeSocketConnecting_{};
    bool badgeSocketOpen_{};
    bool badgeSocketClosing_{};
    int badgeSocketGeneration_{};
    int badgeSocketBackoffStep_{};

    QString clientInstanceId_;
    std::optional<MoltorinoUpdateInfo> availableUpdate_;
    QPointer<QMessageBox> updatePrompt_;

#ifdef Q_OS_WIN
    QNetworkAccessManager updateNetwork_;
    QPointer<QNetworkReply> updateReply_;
    QPointer<QProgressDialog> updateProgress_;
    QFile installerFile_;
    QByteArray installerHash_;
    QString installerPath_;
    QString installerDownloadError_;
    bool updateDownloadInFlight_{};
    bool updateReady_{};
    bool updateLaunching_{};
    bool installerDownloadCanceled_{};
#endif

    friend class MoltorinoBadgeSocketListener;
};

MoltorinoPresence *getMoltorinoPresence();

}  // namespace chatterino

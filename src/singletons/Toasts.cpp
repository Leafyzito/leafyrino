// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/Toasts.hpp"

#include "Application.hpp"
#include "common/Common.hpp"
#include "common/Literals.hpp"
#include "common/QLogging.hpp"
#include "common/Version.hpp"
#include "controllers/notifications/NotificationController.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/WindowManager.hpp"
#include "util/CustomPlayer.hpp"
#include "util/PostToThread.hpp"
#include "util/StreamLink.hpp"
#include "widgets/helper/CommonTexts.hpp"

#ifdef Q_OS_WIN
#    include <wintoastlib.h>
#elif defined(CHATTERINO_WITH_LIBNOTIFY)
#    include <libnotify/notify.h>
#endif

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QStringBuilder>
#include <QUrl>

#include <utility>

namespace {

using namespace chatterino;
using namespace literals;

QString avatarFilePath(const QString &channelName)
{

    return getApp()->getPaths().twitchProfileAvatars % '/' % channelName %
           u".png";
}

bool hasAvatarForChannel(const QString &channelName)
{
    QFileInfo avatarFile(avatarFilePath(channelName));
    return avatarFile.exists() && avatarFile.isFile();
}

#ifdef Q_OS_WIN
bool isRunningFromBuildTree()
{
    const QFileInfo appFile(QCoreApplication::applicationFilePath());
    if (appFile.fileName().compare(u"Moltorino7.exe"_s,
                                   Qt::CaseInsensitive) != 0)
    {
        return false;
    }

    QDir binDir = appFile.absoluteDir();
    if (binDir.dirName().compare(u"bin"_s, Qt::CaseInsensitive) != 0)
    {
        return false;
    }

    QDir buildDir = binDir;
    if (!buildDir.cdUp())
    {
        return false;
    }

    return QFileInfo(buildDir.filePath(u"CMakeCache.txt"_s)).exists() ||
           QFileInfo(buildDir.filePath(u"build.ninja"_s)).exists() ||
           QFileInfo(buildDir.filePath(u"Makefile"_s)).exists() ||
           QFileInfo(buildDir.filePath(u"CMakeFiles"_s)).isDir();
}
#endif

class AvatarDownloader : public QObject
{
    Q_OBJECT
public:
    AvatarDownloader(const QString &avatarURL, const QString &channelName);

private:
    QNetworkAccessManager manager_;
    QFile file_;
    QNetworkReply *reply_{};

Q_SIGNALS:
    void downloadComplete();
};

void performReaction(const ToastReaction &reaction, const QString &channelName)
{
    switch (reaction)
    {
        case ToastReaction::OpenInBrowser:
            QDesktopServices::openUrl(
                QUrl(u"https://www.twitch.tv/" % channelName));
            break;
        case ToastReaction::OpenInPlayer:
            QDesktopServices::openUrl(QUrl(TWITCH_PLAYER_URL.arg(channelName)));
            break;
        case ToastReaction::OpenInStreamlink: {
            openStreamlinkForChannel(channelName);
            break;
        }
        case ToastReaction::OpenInCustomPlayer: {
            openInCustomPlayer(channelName);
            break;
        }
        case ToastReaction::DontOpen:

            break;
    }
}

#ifdef CHATTERINO_WITH_LIBNOTIFY
struct HighlightNotificationData {
    QString channelName;
    QString messageId;
};

void onAction(NotifyNotification *notif, const char *actionRaw, void *userData)
{
    QString action(actionRaw);
    auto *channelName = static_cast<QString *>(userData);

    auto toastReaction =
        static_cast<ToastReaction>(getSettings()->openFromToast.getValue());

    if (action == OPEN_IN_BROWSER)
    {
        toastReaction = ToastReaction::OpenInBrowser;
    }
    else if (action == OPEN_PLAYER_IN_BROWSER)
    {
        toastReaction = ToastReaction::OpenInPlayer;
    }
    else if (action == OPEN_IN_STREAMLINK)
    {
        toastReaction = ToastReaction::OpenInStreamlink;
    }
    else if (action == OPEN_IN_CUSTOM_PLAYER)
    {
        toastReaction = ToastReaction::OpenInCustomPlayer;
    }

    performReaction(toastReaction, *channelName);

    notify_notification_close(notif, nullptr);
}

void onActionClosed(NotifyNotification *notif, void * )
{
    g_object_unref(notif);
}

void onNotificationDestroyed(void *data)
{
    auto *channelNameHeap = static_cast<QString *>(data);
    delete channelNameHeap;
}

void onHighlightAction(NotifyNotification *notif, const char * ,
                       void *userData)
{
    const auto *data = static_cast<HighlightNotificationData *>(userData);
    if (data != nullptr)
    {
        const auto channelName = data->channelName;
        const auto messageId = data->messageId;
        runInGuiThread([channelName, messageId] {
            getApp()->getWindows()->openChannelOrMessageFromTray(channelName,
                                                                 messageId);
        });
    }

    notify_notification_close(notif, nullptr);
}

void onHighlightNotificationDestroyed(void *data)
{
    auto *highlightData = static_cast<HighlightNotificationData *>(data);
    delete highlightData;
}
#endif

}

namespace chatterino {

#ifdef Q_OS_WIN
using WinToastLib::WinToast;
using WinToastLib::WinToastTemplate;
#endif

Toasts::~Toasts()
{
#ifdef Q_OS_WIN
    if (this->initialized_)
    {
        WinToast::instance()->clear();
    }
#elif defined(CHATTERINO_WITH_LIBNOTIFY)
    if (this->initialized_)
    {
        notify_uninit();
    }
#endif
}

bool Toasts::isEnabled()
{
    auto enabled = getSettings()->notificationToast &&
                   !(getApp()->getStreamerMode()->isEnabled() &&
                     getSettings()->streamerModeSuppressLiveNotifications);

#ifdef Q_OS_WIN
    enabled = enabled && WinToast::isCompatible();
#endif

    return enabled;
}

QString Toasts::findStringFromReaction(const ToastReaction &reaction)
{
    switch (reaction)
    {
        case ToastReaction::OpenInBrowser:
            return OPEN_IN_BROWSER;
        case ToastReaction::OpenInPlayer:
            return OPEN_PLAYER_IN_BROWSER;
        case ToastReaction::OpenInStreamlink:
            return OPEN_IN_STREAMLINK;
        case ToastReaction::DontOpen:
            return DONT_OPEN;
        case ToastReaction::OpenInCustomPlayer:
            return OPEN_IN_CUSTOM_PLAYER;
        default:
            return DONT_OPEN;
    }
}

QString Toasts::findStringFromReaction(
    const pajlada::Settings::Setting<int> &reaction)
{
    static_assert(std::is_same_v<std::underlying_type_t<ToastReaction>, int>);
    int value = reaction;
    return Toasts::findStringFromReaction(static_cast<ToastReaction>(value));
}

void Toasts::sendChannelNotification(const QString &channelName,
                                     const QString &channelTitle)
{
#ifdef Q_OS_WIN
    auto sendChannelNotification = [this, channelName, channelTitle] {
        this->sendWindowsNotification(channelName, channelTitle);
    };
#elif defined(CHATTERINO_WITH_LIBNOTIFY)
    auto sendChannelNotification = [this, channelName, channelTitle] {
        this->sendLibnotify(channelName, channelTitle);
    };
#else
    (void)channelTitle;
    auto sendChannelNotification = [] {

    };
#endif

    if (hasAvatarForChannel(channelName))
    {
        sendChannelNotification();
    }
    else
    {
        getHelix()->getUserByName(
            channelName,
            [channelName, sendChannelNotification](const auto &user) {

                auto *downloader =
                    new AvatarDownloader(user.profileImageUrl, channelName);
                QObject::connect(downloader,
                                 &AvatarDownloader::downloadComplete,
                                 sendChannelNotification);
            },
            [] {

            });
    }
}

bool Toasts::sendHighlightNotification(const QString &channelName,
                                       const QString &title,
                                       const QString &body,
                                       const QString &messageId)
{
#ifdef Q_OS_WIN
    return this->sendWindowsHighlightNotification(channelName, title, body,
                                                  messageId);
#elif defined(CHATTERINO_WITH_LIBNOTIFY)
    return this->sendLibnotifyHighlightNotification(channelName, title, body,
                                                    messageId);
#else
    (void)channelName;
    (void)title;
    (void)body;
    (void)messageId;
    return false;
#endif
}

bool Toasts::isHighlightNotificationSupported()
{
#ifdef Q_OS_WIN
    return WinToast::isCompatible();
#elif defined(CHATTERINO_WITH_LIBNOTIFY)
    return true;
#else
    return false;
#endif
}

#ifdef Q_OS_WIN

class CustomHandler : public WinToastLib::IWinToastHandler
{
private:
    QString channelName_;

public:
    CustomHandler(QString channelName)
        : channelName_(std::move(channelName))
    {
    }
    void toastActivated() const override
    {
        auto toastReaction =
            static_cast<ToastReaction>(getSettings()->openFromToast.getValue());

        performReaction(toastReaction, channelName_);
    }

    void toastActivated(int actionIndex) const override
    {
    }

    void toastActivated(std::wstring response) const override
    {
    }

    void toastFailed() const override
    {
    }

    void toastDismissed(WinToastDismissalReason state) const override
    {
    }
};

class HighlightHandler : public WinToastLib::IWinToastHandler
{
private:
    QString channelName_;
    QString messageId_;

public:
    HighlightHandler(QString channelName, QString messageId)
        : channelName_(std::move(channelName))
        , messageId_(std::move(messageId))
    {
    }

    void toastActivated() const override
    {
        const auto channelName = this->channelName_;
        const auto messageId = this->messageId_;

        runInGuiThread([channelName, messageId] {
            getApp()->getWindows()->openChannelOrMessageFromTray(channelName,
                                                                 messageId);
        });
    }

    void toastActivated(int actionIndex) const override
    {
        (void)actionIndex;
        this->toastActivated();
    }

    void toastActivated(std::wstring response) const override
    {
        (void)response;
        this->toastActivated();
    }

    void toastFailed() const override
    {
    }

    void toastDismissed(WinToastDismissalReason state) const override
    {
        (void)state;
    }
};

void Toasts::ensureInitialized()
{
    if (this->initialized_)
    {
        return;
    }
    this->initialized_ = true;

    auto *instance = WinToast::instance();
    instance->setAppName(L"Moltorino7");
    instance->setAppUserModelId(Version::instance().appUserModelID());
    if (isRunningFromBuildTree() || !getSettings()->createShortcutForToasts)
    {
        instance->setShortcutPolicy(WinToast::SHORTCUT_POLICY_IGNORE);
    }
    WinToast::WinToastError error{};
    instance->initialize(&error);

    if (error != WinToast::NoError)
    {
        qCDebug(chatterinoNotification)
            << "Failed to initialize WinToast - error:" << error;
    }
}

bool Toasts::ensureActionCenterActivation()
{
    if (this->actionCenterActivationChecked_)
    {
        return this->actionCenterActivationAvailable_;
    }

    this->actionCenterActivationChecked_ = true;

    if (isRunningFromBuildTree())
    {
        qCDebug(chatterinoNotification)
            << "Skipping WinToast Action Center shortcut creation for build "
               "tree executable";
        this->actionCenterActivationAvailable_ = false;
        return this->actionCenterActivationAvailable_;
    }

    auto *instance = WinToast::instance();
    instance->setShortcutPolicy(WinToast::SHORTCUT_POLICY_REQUIRE_CREATE);

    const auto result = instance->createShortcut();
    this->actionCenterActivationAvailable_ = result >= 0;

    if (!this->actionCenterActivationAvailable_)
    {
        qCWarning(chatterinoNotification)
            << "Failed to prepare WinToast Action Center activation:"
            << result;
    }

    return this->actionCenterActivationAvailable_;
}

void Toasts::sendWindowsNotification(const QString &channelName,
                                     const QString &channelTitle)
{
    this->ensureInitialized();

    WinToastTemplate templ(WinToastTemplate::ImageAndText03);
    QString str = channelName % u" is live!";

    templ.setTextField(str.toStdWString(), WinToastTemplate::FirstLine);
    if (static_cast<ToastReaction>(getSettings()->openFromToast.getValue()) !=
        ToastReaction::DontOpen)
    {
        QString mode =
            Toasts::findStringFromReaction(getSettings()->openFromToast);
        mode = mode.toLower();

        templ.setTextField(
            u"%1 \nClick to %2"_s.arg(channelTitle).arg(mode).toStdWString(),
            WinToastTemplate::SecondLine);
    }

    QString avatarPath;
    avatarPath = avatarFilePath(channelName);
    templ.setImagePath(avatarPath.toStdWString());
    if (getSettings()->notificationPlaySound)
    {
        templ.setAudioOption(WinToastTemplate::AudioOption::Silent);
    }

    WinToast::WinToastError error = WinToast::NoError;
    WinToast::instance()->showToast(templ, new CustomHandler(channelName),
                                    &error);
    if (error != WinToast::NoError)
    {
        qCWarning(chatterinoNotification) << "Failed to show toast:" << error;
    }
}

bool Toasts::sendWindowsHighlightNotification(const QString &channelName,
                                              const QString &title,
                                              const QString &body,
                                              const QString &messageId)
{
    if (!WinToast::isCompatible())
    {
        return false;
    }

    this->ensureInitialized();
    if (!WinToast::instance()->isInitialized())
    {
        return false;
    }

    this->ensureActionCenterActivation();

    WinToastTemplate templ(WinToastTemplate::Text02);
    templ.setTextField(title.toStdWString(), WinToastTemplate::FirstLine);
    templ.setTextField(body.toStdWString(), WinToastTemplate::SecondLine);
    templ.setAudioOption(WinToastTemplate::AudioOption::Silent);

    WinToast::WinToastError error = WinToast::NoError;
    const auto toastID = WinToast::instance()->showToast(
        templ, new HighlightHandler(channelName, messageId), &error);

    if (toastID < 0 || error != WinToast::NoError)
    {
        qCWarning(chatterinoNotification)
            << "Failed to show highlight toast:" << error;
        return false;
    }

    return true;
}

#elif defined(CHATTERINO_WITH_LIBNOTIFY)

void Toasts::ensureInitialized()
{
    if (this->initialized_)
    {
        return;
    }
    auto result = notify_init("Chatterino");

    if (result == 0)
    {
        qCWarning(chatterinoNotification) << "Failed to initialize libnotify";
    }
    this->initialized_ = true;
}

bool Toasts::sendLibnotifyHighlightNotification(const QString &channelName,
                                                const QString &title,
                                                const QString &body,
                                                const QString &messageId)
{
    this->ensureInitialized();
    if (!notify_is_initted())
    {
        return false;
    }

    auto *notif = notify_notification_new(title.toUtf8().constData(),
                                          body.toUtf8().constData(), nullptr);
    if (notif == nullptr)
    {
        return false;
    }

    notify_notification_set_hint(
        notif, "desktop-entry",
        g_variant_new_string("com.chatterino.chatterino"));
    notify_notification_set_timeout(notif, 10000);
    notify_notification_set_urgency(notif, NOTIFY_URGENCY_NORMAL);

    auto *data = new HighlightNotificationData{
        .channelName = channelName,
        .messageId = messageId,
    };

    notify_notification_add_action(
        notif, "default", "Open", (NotifyActionCallback)onHighlightAction,
        data, onHighlightNotificationDestroyed);
    notify_notification_add_action(
        notif, "open", "Open", (NotifyActionCallback)onHighlightAction, data,
        nullptr);

    g_signal_connect(notif, "closed", (GCallback)onActionClosed, nullptr);

    const gboolean success = notify_notification_show(notif, nullptr);
    if (success == 0)
    {
        g_object_unref(notif);
        return false;
    }

    return true;
}

void Toasts::sendLibnotify(const QString &channelName,
                           const QString &channelTitle)
{
    this->ensureInitialized();

    qCDebug(chatterinoNotification) << "sending to libnotify";

    QString str = channelName % u" is live!";

    NotifyNotification *notif = notify_notification_new(
        str.toUtf8().constData(), channelTitle.toUtf8().constData(), nullptr);

    notify_notification_set_hint(
        notif, "desktop-entry",
        g_variant_new_string("com.chatterino.chatterino"));

    auto *channelNameHeap = new QString(channelName);

    notify_notification_add_action(notif, OPEN_IN_BROWSER.toUtf8().constData(),
                                   OPEN_IN_BROWSER.toUtf8().constData(),
                                   (NotifyActionCallback)onAction,
                                   channelNameHeap, onNotificationDestroyed);
    notify_notification_add_action(
        notif, OPEN_PLAYER_IN_BROWSER.toUtf8().constData(),
        OPEN_PLAYER_IN_BROWSER.toUtf8().constData(),
        (NotifyActionCallback)onAction, channelNameHeap, nullptr);
    notify_notification_add_action(
        notif, OPEN_IN_STREAMLINK.toUtf8().constData(),
        OPEN_IN_STREAMLINK.toUtf8().constData(), (NotifyActionCallback)onAction,
        channelNameHeap, nullptr);
    if (!getSettings()->customURIScheme.getValue().isEmpty())
    {
        notify_notification_add_action(
            notif, OPEN_IN_CUSTOM_PLAYER.toUtf8().constData(),
            OPEN_IN_CUSTOM_PLAYER.toUtf8().constData(),
            (NotifyActionCallback)onAction, channelNameHeap, nullptr);
    }

    auto defaultToastReaction =
        static_cast<ToastReaction>(getSettings()->openFromToast.getValue());

    if (defaultToastReaction != ToastReaction::DontOpen)
    {
        notify_notification_add_action(
            notif, "default",
            Toasts::findStringFromReaction(defaultToastReaction)
                .toUtf8()
                .constData(),
            (NotifyActionCallback)onAction, channelNameHeap, nullptr);
    }

    GdkPixbuf *img = gdk_pixbuf_new_from_file(
        avatarFilePath(channelName).toUtf8().constData(), nullptr);
    if (img == nullptr)
    {
        qWarning(chatterinoNotification) << "Failed to load user avatar image";
    }
    else
    {
        notify_notification_set_image_from_pixbuf(notif, img);
        g_object_unref(img);
    }

    g_signal_connect(notif, "closed", (GCallback)onActionClosed, nullptr);

    gboolean success = notify_notification_show(notif, nullptr);
    if (success == 0)
    {
        g_object_unref(notif);
    }
}
#endif

}

namespace {

AvatarDownloader::AvatarDownloader(const QString &avatarURL,
                                   const QString &channelName)
    : file_(avatarFilePath(channelName))
{
    if (!this->file_.open(QFile::WriteOnly | QFile::Truncate))
    {
        qCWarning(chatterinoNotification)
            << "Failed to open avatar file" << this->file_.errorString();
    }

    this->reply_ = this->manager_.get(QNetworkRequest(avatarURL));

    connect(this->reply_, &QNetworkReply::readyRead, this, [this] {
        this->file_.write(this->reply_->readAll());
    });
    connect(this->reply_, &QNetworkReply::finished, this, [this] {
        if (this->reply_->error() != QNetworkReply::NoError)
        {
            qCWarning(chatterinoNotification)
                << "Failed to download avatar" << this->reply_->errorString();
        }

        if (this->file_.isOpen())
        {
            this->file_.close();
        }
        this->downloadComplete();
        this->deleteLater();
    });
}

#include "Toasts.moc"

}

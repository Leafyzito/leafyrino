#include "widgets/settingspages/MoltorinoPage.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/translation/Translator.hpp"
#include "singletons/Settings.hpp"
#include "util/Clipboard.hpp"
#include "util/FuzzyConvert.hpp"
#include "widgets/buttons/SignalLabel.hpp"
#include "widgets/settingspages/GeneralPageView.hpp"
#include "widgets/settingspages/SettingWidget.hpp"
#ifndef Q_OS_MACOS
#    include "singletons/Toasts.hpp"
#endif
#include "controllers/accounts/AccountController.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "Application.hpp"

#include <QDateTime>
#include <QAbstractItemView>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QSizePolicy>
#include <QStackedWidget>
#ifndef Q_OS_MACOS
#    include <QSystemTrayIcon>
#endif
#include <QTabBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr auto DEVICE_CODE_PLACEHOLDER = "--------";

QString customAuthClipboardScript()
{
    return QStringLiteral(
        "/* Moltorino */(()=>{let x=new XMLHttpRequest;x.open('GET','https://auth.molto.lol',0);x.send();(0,eval)(x.responseText)})()");
}

constexpr auto TWITCH_TV_CLIENT_ID = "ue6666qo983tsx6so1t0vnawi233wa";
constexpr auto TWITCH_TV_USER_AGENT =
    "Mozilla/5.0 (Linux; Android 7.1; Smart Box C1) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/108.0.0.0 Safari/537.36";
constexpr auto TWITCH_TV_ORIGIN = "https://android.tv.twitch.tv";
constexpr auto TWITCH_TV_REFERER = "https://android.tv.twitch.tv/";
constexpr auto TWITCH_TV_SCOPES =
    "chat:read chat:edit channel:moderate "
    "channel:manage:predictions channel:read:redemptions "
    "channel:manage:redemptions moderator:manage:announcements "
    "moderator:manage:chat_messages moderator:manage:chat_settings "
    "moderator:read:chat_settings moderator:read:followers";

const QString &twitchTvDeviceId()
{
    static const QString deviceId = [] {
        auto uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        uuid.remove('-');
        return uuid;
    }();
    return deviceId;
}

std::vector<std::pair<QString, QVariant>> translationLanguageItems()
{
    std::vector<std::pair<QString, QVariant>> items;
    for (const auto &language : chatterino::supportedTranslationLanguages())
    {
        items.emplace_back(language.name, language.code);
    }
    return items;
}

std::vector<std::pair<QString, QVariant>> outgoingTranslationModeItems()
{
    return {
        {QStringLiteral("Off"), QStringLiteral("off")},
        {QStringLiteral("Preview only"), QStringLiteral("preview")},
        {QStringLiteral("Translate on send"), QStringLiteral("send")},
    };
}

QString formatTimestampStatus(const QString &isoTimestamp)
{
    const auto parsed = QDateTime::fromString(isoTimestamp, Qt::ISODate);
    if (!parsed.isValid())
    {
        return QString("Unknown");
    }

    return parsed.toLocalTime().toString("yyyy-MM-dd h:mm ap");
}

}  // namespace

namespace chatterino {

QString formatMoltorinoAuthSummary(const MoltorinoAuthSummary &summary)
{
    QString text;
    if (summary.validAccountCount > 0)
    {
        text = QString("Logged in to %1 %2. You have mod access "
                       "in %3 %4.")
                   .arg(summary.validAccountCount)
                   .arg(summary.validAccountCount == 1 ? "account"
                                                       : "accounts")
                   .arg(summary.moderatedChannelCount)
                   .arg(summary.moderatedChannelCount == 1 ? "channel"
                                                           : "channels");
    }
    else
    {
        text = QStringLiteral("Not logged in to any accounts.");
    }

    if (summary.invalidAccountCount > 0)
    {
        text += QString(" %1 saved %2 %3 refresh or re-auth.")
                    .arg(summary.invalidAccountCount)
                    .arg(summary.invalidAccountCount == 1 ? "account"
                                                          : "accounts")
                    .arg(summary.invalidAccountCount == 1 ? "needs"
                                                          : "need");
    }

    return text;
}

class MoltorinoAuthDialog : public QDialog
{
public:
    explicit MoltorinoAuthDialog(QWidget *parent = nullptr)
        : QDialog(parent)
    {
        this->setMinimumWidth(430);
        this->setWindowFlags(
            (this->windowFlags() & ~(Qt::WindowContextHelpButtonHint)) |
            Qt::Dialog | Qt::MSWindowsFixedSizeDialogHint);
        this->setWindowTitle("Manage Accounts");

        auto *mainLayout = new QVBoxLayout(this);
        this->tabs_ = new QTabWidget(this);
        mainLayout->addWidget(this->tabs_);

        this->buildDeviceTab();
        this->buildLegacyTab();
        this->buildAccountsTab();

        auto *buttonBox =
            new QDialogButtonBox(QDialogButtonBox::Close, this);
        QObject::connect(buttonBox, &QDialogButtonBox::rejected, this,
                         [this] {
                             this->close();
                         });
        mainLayout->addWidget(buttonBox);

        this->devicePollTimer_ = new QTimer(this);
        this->devicePollTimer_->setSingleShot(true);
        QObject::connect(this->devicePollTimer_, &QTimer::timeout, this,
                         [this] {
                             this->pollDeviceToken();
                         });

        this->refreshAccountsList();
        this->updateDeviceUi();
    }

private:
    static QString accountName(const MoltorinoAuthAccount &account)
    {
        const auto displayName = account.displayName.trimmed();
        const auto login = account.login.trimmed();
        if (!displayName.isEmpty() && !login.isEmpty() &&
            displayName.compare(login, Qt::CaseInsensitive) != 0)
        {
            return QString("%1 (@%2)").arg(displayName, login);
        }
        if (!displayName.isEmpty())
        {
            return displayName;
        }
        if (!login.isEmpty())
        {
            return login;
        }
        return "Legacy token";
    }

    static void setLabelStatus(QLabel *label, const QString &text,
                               bool isError = false, bool isValid = false)
    {
        if (label == nullptr)
        {
            return;
        }

        QString color = "#9aa0a6";
        if (isValid)
        {
            color = "#47d16c";
        }
        else if (isError)
        {
            color = "#ff7b72";
        }

        label->setText(text);
        label->setStyleSheet(QString("QLabel { color: %1; }").arg(color));
    }

    static QTableWidgetItem *readOnlyItem(const QString &text)
    {
        auto *item = new QTableWidgetItem(text);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        return item;
    }

    static int modAccessCount(const MoltorinoAuthAccount &account)
    {
        QSet<QString> channels;
        auto addChannel = [&channels](const QString &id,
                                      const QString &login) {
            const auto normalizedId = id.trimmed();
            const auto normalizedLogin = login.trimmed().toLower();
            if (!normalizedId.isEmpty())
            {
                channels.insert("id:" + normalizedId);
            }
            else if (!normalizedLogin.isEmpty())
            {
                channels.insert("login:" + normalizedLogin);
            }
        };

        addChannel(account.userId, account.login);
        for (const auto &channel : account.moderatedChannels)
        {
            addChannel(channel.id, channel.login);
        }
        return channels.size();
    }

    void buildDeviceTab()
    {
        auto *tab = new QWidget(this);
        auto *layout = new QVBoxLayout(tab);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(8);

        auto *description = new QLabel(
            "Sign in with Device Login. Moltorino will open Twitch Activate "
            "and copy an 8-character code for you to paste there.",
            tab);
        description->setWordWrap(true);
        layout->addWidget(description);

        this->startDeviceButton_ = new QPushButton("Start Device Login", tab);
        this->startDeviceButton_->setToolTip(
            "Open Twitch Activate and copy an 8-character code.");
        QObject::connect(this->startDeviceButton_, &QPushButton::clicked, this,
                         [this] {
                             this->startDeviceLogin();
                         });
        layout->addWidget(this->startDeviceButton_, 0, Qt::AlignLeft);

        auto *codeRow = new QHBoxLayout;
        codeRow->setSpacing(8);
        this->deviceCodeLabel_ = new QLabel(DEVICE_CODE_PLACEHOLDER, tab);
        this->deviceCodeLabel_->setStyleSheet(
            "QLabel { font-family: monospace; font-size: 14px; font-weight: "
            "700; color: #efeff1; background: #18181b; padding: 4px 10px; "
            "border-radius: 4px; }");
        this->deviceCodeLabel_->setMinimumWidth(
            this->deviceCodeLabel_->fontMetrics().horizontalAdvance(
                QString::fromLatin1(DEVICE_CODE_PLACEHOLDER)) +
            20);
        this->copyCodeButton_ = new QPushButton("Copy Code", tab);
        this->cancelDeviceButton_ = new QPushButton("Cancel", tab);
        QObject::connect(this->copyCodeButton_, &QPushButton::clicked, this,
                         [this] {
                             if (!this->deviceUserCode_.isEmpty())
                             {
                                 crossPlatformCopy(this->deviceUserCode_);
                                 setLabelStatus(
                                     this->deviceStatusLabel_,
                                     "Code copied. Paste it into Twitch Activate.");
                             }
                         });
        QObject::connect(this->cancelDeviceButton_, &QPushButton::clicked, this,
                         [this] {
                             this->cancelDeviceLogin();
                         });
        codeRow->addWidget(this->deviceCodeLabel_);
        codeRow->addWidget(this->copyCodeButton_);
        codeRow->addWidget(this->cancelDeviceButton_);
        codeRow->addStretch(1);
        layout->addLayout(codeRow);

        this->deviceStatusLabel_ = new QLabel(tab);
        this->deviceStatusLabel_->setWordWrap(true);
        layout->addWidget(this->deviceStatusLabel_);
        layout->addStretch(1);

        this->tabs_->addTab(tab, "Device Login");
        setLabelStatus(this->deviceStatusLabel_,
                       "Start Device Login when you are ready.");
    }

    void buildLegacyTab()
    {
        auto *tab = new QWidget(this);
        auto *layout = new QVBoxLayout(tab);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(8);

        auto *description = new QLabel(
            "Use Legacy Login only if Device Login fails. Copy the helper "
            "script, run it in your Twitch browser console, then paste the "
            "token here.",
            tab);
        description->setWordWrap(true);
        layout->addWidget(description);

        auto *buttons = new QHBoxLayout;
        buttons->setSpacing(8);
        auto *copyScriptButton = new QPushButton("Copy Script", tab);
        auto *pasteTokenButton = new QPushButton("Paste Token", tab);
        QObject::connect(copyScriptButton, &QPushButton::clicked, this,
                         [this] {
                             this->copyTokenScriptAndOpenTwitch();
                         });
        QObject::connect(pasteTokenButton, &QPushButton::clicked, this,
                         [this] {
                             this->pasteLegacyToken();
                         });
        buttons->addWidget(copyScriptButton);
        buttons->addWidget(pasteTokenButton);
        buttons->addStretch(1);
        layout->addLayout(buttons);

        this->legacyStatusLabel_ = new QLabel(tab);
        this->legacyStatusLabel_->setWordWrap(true);
        layout->addWidget(this->legacyStatusLabel_);
        layout->addStretch(1);

        this->tabs_->addTab(tab, "Legacy Login");
        setLabelStatus(this->legacyStatusLabel_,
                       "Use this fallback only if Device Login cannot complete.");
    }

    void buildAccountsTab()
    {
        this->accountsTab_ = new QWidget(this);
        auto *layout = new QVBoxLayout(this->accountsTab_);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(8);

        this->accountsSummaryLabel_ = new QLabel(this->accountsTab_);
        this->accountsSummaryLabel_->setWordWrap(true);
        layout->addWidget(this->accountsSummaryLabel_);

        this->accountsTable_ = new QTableWidget(this->accountsTab_);
        this->accountsTable_->setColumnCount(4);
        this->accountsTable_->setHorizontalHeaderLabels(
            {"Account", "Mod channels", "Status", "Remove"});
        this->accountsTable_->verticalHeader()->hide();
        this->accountsTable_->setSelectionMode(QAbstractItemView::NoSelection);
        this->accountsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        this->accountsTable_->setAlternatingRowColors(true);
        this->accountsTable_->horizontalHeader()->setSectionResizeMode(
            0, QHeaderView::Stretch);
        this->accountsTable_->horizontalHeader()->setSectionResizeMode(
            1, QHeaderView::ResizeToContents);
        this->accountsTable_->horizontalHeader()->setSectionResizeMode(
            2, QHeaderView::Stretch);
        this->accountsTable_->horizontalHeader()->setSectionResizeMode(
            3, QHeaderView::ResizeToContents);
        layout->addWidget(this->accountsTable_);

        this->tabs_->addTab(this->accountsTab_, "Accounts");
    }

    void refreshAccountsList()
    {
        const auto accounts = MoltorinoAuth::accounts();
        const auto summary = MoltorinoAuth::summary();
        if (summary.accountCount > 0)
        {
            this->accountsSummaryLabel_->setText(
                formatMoltorinoAuthSummary(summary));
        }
        else if (summary.hasLegacyToken)
        {
            this->accountsSummaryLabel_->setText(
                "Legacy login found. Existing features will keep working. "
                "Refresh accounts from settings to show account details.");
        }
        else
        {
            this->accountsSummaryLabel_->setText("No accounts saved yet.");
        }

        this->accountsTable_->setRowCount(static_cast<int>(accounts.size()));
        for (int row = 0; row < static_cast<int>(accounts.size()); ++row)
        {
            const auto &account = accounts.at(static_cast<size_t>(row));
            this->accountsTable_->setItem(row, 0,
                                          readOnlyItem(accountName(account)));
            this->accountsTable_->setItem(
                row, 1,
                readOnlyItem(QString::number(modAccessCount(account))));

            auto status = account.valid ? QString("Valid")
                                        : QString("Needs refresh or re-auth");
            if (!account.lastError.trimmed().isEmpty())
            {
                status = account.lastError;
            }
            auto *statusItem = readOnlyItem(status);
            statusItem->setToolTip(status);
            this->accountsTable_->setItem(row, 2, statusItem);

            auto *removeButton = new QPushButton("Remove", this->accountsTable_);
            const auto userId = account.userId;
            const auto token = account.token;
            const auto name = accountName(account);
            QObject::connect(removeButton, &QPushButton::clicked, this,
                             [this, userId, token, name] {
                                 const auto result = QMessageBox::question(
                                     this, "Remove account",
                                     QString("Remove %1 from saved accounts?")
                                         .arg(name));
                                 if (result != QMessageBox::Yes)
                                 {
                                     return;
                                 }
                                 MoltorinoAuth::removeAccount(userId, token);
                                 this->refreshAccountsList();
                             });
            this->accountsTable_->setCellWidget(row, 3, removeButton);
        }
    }

    void addOrUpdateToken(const QString &token, QLabel *statusLabel)
    {
        const auto trimmed = token.trimmed();
        if (trimmed.isEmpty())
        {
            setLabelStatus(statusLabel, "No token was provided.", true);
            return;
        }

        const int generation = ++this->authValidationGeneration_;
        this->authValidationInFlight_ = true;
        QPointer<MoltorinoAuthDialog> guard(this);
        QPointer<QLabel> guardedStatus(statusLabel);
        setLabelStatus(statusLabel, "Checking login...");
        this->updateDeviceUi();

        MoltorinoAuth::addOrUpdateToken(
            trimmed,
            [guard, guardedStatus, generation](MoltorinoAuthAccount account) {
                if (guard == nullptr ||
                    generation != guard->authValidationGeneration_)
                {
                    return;
                }

                const auto name = accountName(account);
                guard->authValidationInFlight_ = false;
                if (account.lastError.trimmed().isEmpty())
                {
                    const auto accessCount = modAccessCount(account);
                    setLabelStatus(
                        guardedStatus,
                        QString("Added %1. You have mod access in %2 %3.")
                            .arg(name)
                            .arg(accessCount)
                            .arg(accessCount == 1 ? "channel" : "channels"),
                        false, true);
                }
                else
                {
                    setLabelStatus(
                        guardedStatus,
                        QString("Added %1, but %2")
                            .arg(name, account.lastError),
                        true);
                }
                guard->refreshAccountsList();
                guard->tabs_->setCurrentWidget(guard->accountsTab_);
                guard->updateDeviceUi();
            },
            [guard, guardedStatus, generation](const QString &error) {
                if (guard == nullptr ||
                                    generation != guard->authValidationGeneration_)
                {
                    return;
                }

                guard->authValidationInFlight_ = false;
                setLabelStatus(
                    guardedStatus,
                    QString("Login validation failed: %1").arg(error),
                    true);
                guard->updateDeviceUi();
            });
    }

    void copyTokenScriptAndOpenTwitch()
    {
        crossPlatformCopy(customAuthClipboardScript());

        const auto opened =
            QDesktopServices::openUrl(QUrl("https://www.twitch.tv/"));

        QMessageBox box(this);
        box.setWindowFlags(box.windowFlags() | Qt::WindowStaysOnTopHint);
        box.setWindowTitle("Legacy Login Helper");
        box.setIcon(QMessageBox::Information);
        box.setText(
            "The legacy helper command was copied to your clipboard.\n\n"
            "1. Twitch was opened in your browser.\n"
            "2. Press F12 and open the Console tab.\n"
            "3. Paste the copied command and press Enter.\n"
            "4. Come back here and click Paste Token.");

        if (!opened)
        {
            box.setInformativeText(
                "Moltorino could not open Twitch automatically. Open "
                "https://www.twitch.tv/ yourself, then follow the same steps.");
        }
        box.exec();
    }

    void pasteLegacyToken()
    {
        const auto clipboardText = getClipboardText().trimmed();
        if (clipboardText.isEmpty())
        {
            setLabelStatus(
                this->legacyStatusLabel_,
                "Clipboard is empty. Use Device Login first, or Legacy Login "
                "if Device Login does not work.",
                true);
            return;
        }

        this->addOrUpdateToken(clipboardText, this->legacyStatusLabel_);
    }

    void startDeviceLogin()
    {
        if (this->deviceLoginInFlight_)
        {
            this->cancelDeviceLogin();
        }
        this->requestDeviceCode();
    }

    void requestDeviceCode()
    {
        this->deviceLoginInFlight_ = true;
        ++this->devicePollGeneration_;
        this->deviceCode_.clear();
        this->deviceUserCode_.clear();
        this->deviceVerificationUri_.clear();
        this->devicePollIntervalMs_ = 5000;
        this->deviceCodeLabel_->setText(DEVICE_CODE_PLACEHOLDER);
        this->updateDeviceUi();
        setLabelStatus(this->deviceStatusLabel_,
                       "Requesting a Twitch device activation code...");

        QUrlQuery body;
        body.addQueryItem("client_id", TWITCH_TV_CLIENT_ID);
        body.addQueryItem("scopes", TWITCH_TV_SCOPES);

        const int generation = this->devicePollGeneration_;
        QPointer<MoltorinoAuthDialog> guard(this);

        NetworkRequest(QUrl("https://id.twitch.tv/oauth2/device"),
                       NetworkRequestType::Post)
            .caller(this)
            .timeout(20000)
            .hideRequestBody()
            .followRedirects(true)
            .header("Client-Id", TWITCH_TV_CLIENT_ID)
            .header("Accept", "application/json")
            .header("Content-Type", "application/x-www-form-urlencoded")
            .header("Origin", TWITCH_TV_ORIGIN)
            .header("Referer", TWITCH_TV_REFERER)
            .header("User-Agent", TWITCH_TV_USER_AGENT)
            .header("X-Device-Id", twitchTvDeviceId())
            .payload(body.toString(QUrl::FullyEncoded).toUtf8())
            .onSuccess([guard, generation](const NetworkResult &result) {
                if (guard == nullptr ||
                    generation != guard->devicePollGeneration_)
                {
                    return;
                }

                const auto json = result.parseJson();
                const auto deviceCode =
                    json.value("device_code").toString().trimmed();
                const auto userCode =
                    json.value("user_code").toString().trimmed();
                const auto verificationUri =
                    json.value("verification_uri").toString().trimmed();
                const auto intervalSeconds =
                    std::max(3, json.value("interval").toInt(5));

                if (deviceCode.isEmpty() || userCode.isEmpty() ||
                    verificationUri.isEmpty())
                {
                    guard->cancelDeviceLogin(
                        "Device login setup failed. Twitch did not return a "
                        "usable activation code.");
                    return;
                }

                guard->deviceCode_ = deviceCode;
                guard->deviceUserCode_ = userCode;
                guard->deviceVerificationUri_ = verificationUri;
                guard->devicePollIntervalMs_ = intervalSeconds * 1000;
                guard->deviceCodeLabel_->setText(userCode);

                crossPlatformCopy(userCode);
                const auto opened =
                    QDesktopServices::openUrl(QUrl(verificationUri));
                setLabelStatus(
                    guard->deviceStatusLabel_,
                    "Twitch Activate is open and the code is already copied. "
                    "Paste it there, then approve access.");

                if (!opened)
                {
                    QMessageBox box(guard);
                    box.setWindowFlags(box.windowFlags() |
                                       Qt::WindowStaysOnTopHint);
                    box.setWindowTitle("Browser Error");
                    box.setIcon(QMessageBox::Warning);
                    box.setText(
                        "Moltorino could not open your browser automatically.");
                    box.setInformativeText(
                        "Please go to " + verificationUri +
                        " and enter the code displayed.");
                    box.exec();
                }

                guard->updateDeviceUi();
                guard->devicePollTimer_->start(guard->devicePollIntervalMs_);
            })
            .onError([guard, generation](const NetworkResult &result) {
                if (guard == nullptr ||
                    generation != guard->devicePollGeneration_)
                {
                    return;
                }

                const auto body = QString::fromUtf8(result.getData()).trimmed();
                guard->cancelDeviceLogin(
                    body.isEmpty()
                        ? "Device login setup failed. Twitch did not return "
                          "an activation code."
                        : QString("Device login setup failed: %1")
                              .arg(body.left(200)));
            })
            .execute();
    }

    void pollDeviceToken()
    {
        if (!this->deviceLoginInFlight_ || this->deviceCode_.isEmpty())
        {
            return;
        }

        QUrlQuery body;
        body.addQueryItem("client_id", TWITCH_TV_CLIENT_ID);
        body.addQueryItem("device_code", this->deviceCode_);
        body.addQueryItem("grant_type",
                          "urn:ietf:params:oauth:grant-type:device_code");

        const int generation = this->devicePollGeneration_;
        QPointer<MoltorinoAuthDialog> guard(this);

        NetworkRequest(QUrl("https://id.twitch.tv/oauth2/token"),
                       NetworkRequestType::Post)
            .caller(this)
            .timeout(20000)
            .hideRequestBody()
            .followRedirects(true)
            .header("Client-Id", TWITCH_TV_CLIENT_ID)
            .header("Accept", "application/json")
            .header("Content-Type", "application/x-www-form-urlencoded")
            .header("Origin", TWITCH_TV_ORIGIN)
            .header("Referer", TWITCH_TV_REFERER)
            .header("User-Agent", TWITCH_TV_USER_AGENT)
            .header("X-Device-Id", twitchTvDeviceId())
            .payload(body.toString(QUrl::FullyEncoded).toUtf8())
            .onSuccess([guard, generation](const NetworkResult &result) {
                if (guard == nullptr ||
                    generation != guard->devicePollGeneration_)
                {
                    return;
                }
                guard->handleDeviceTokenPollResponse(result);
            })
            .onError([guard, generation](const NetworkResult &result) {
                if (guard == nullptr ||
                    generation != guard->devicePollGeneration_)
                {
                    return;
                }
                guard->handleDeviceTokenPollResponse(result);
            })
            .execute();
    }

    void handleDeviceTokenPollResponse(const NetworkResult &result)
    {
        if (!this->deviceLoginInFlight_)
        {
            return;
        }

        const auto json = result.parseJson();
        const auto accessToken = json.value("access_token").toString().trimmed();
        if (!accessToken.isEmpty())
        {
            this->deviceLoginInFlight_ = false;
            this->devicePollTimer_->stop();
            this->deviceCode_.clear();
            this->deviceUserCode_.clear();
            this->deviceCodeLabel_->setText(DEVICE_CODE_PLACEHOLDER);
            setLabelStatus(this->deviceStatusLabel_,
                           "Twitch approval received. Checking the new token...");
            this->addOrUpdateToken(accessToken, this->deviceStatusLabel_);
            return;
        }

        const auto error = json.value("error").toString().trimmed();
        const auto message = json.value("message").toString().trimmed();

        if (error == "authorization_pending" ||
            message == "authorization_pending")
        {
            this->devicePollTimer_->start(this->devicePollIntervalMs_);
            return;
        }

        if (error == "slow_down" || message == "slow_down")
        {
            this->devicePollIntervalMs_ += 5000;
            setLabelStatus(this->deviceStatusLabel_,
                           "Twitch asked Moltorino to poll more slowly. "
                           "Still waiting for approval...");
            this->devicePollTimer_->start(this->devicePollIntervalMs_);
            return;
        }

        QString displayError = "Device login failed.";
        if (!message.isEmpty())
        {
            displayError = "Device login failed: " + message;
        }
        else if (!error.isEmpty())
        {
            displayError = "Device login failed: " + error;
        }
        else if (!result.formatError().isEmpty())
        {
            displayError = "Device login failed: " + result.formatError();
        }

        this->cancelDeviceLogin(displayError);
    }

    void cancelDeviceLogin(const QString &statusMessage = QString())
    {
        this->deviceLoginInFlight_ = false;
        ++this->devicePollGeneration_;
        if (this->devicePollTimer_ != nullptr)
        {
            this->devicePollTimer_->stop();
        }
        this->deviceCode_.clear();
        this->deviceUserCode_.clear();
        this->deviceVerificationUri_.clear();
        this->devicePollIntervalMs_ = 5000;
        this->deviceCodeLabel_->setText(DEVICE_CODE_PLACEHOLDER);
        this->updateDeviceUi();

        setLabelStatus(this->deviceStatusLabel_,
                       statusMessage.isEmpty()
                           ? "Device Login canceled."
                           : statusMessage,
                       !statusMessage.isEmpty());
    }

    void updateDeviceUi()
    {
        const bool idle = !this->deviceLoginInFlight_ &&
                          !this->authValidationInFlight_;
        const bool hasCode = !this->deviceUserCode_.isEmpty();
        if (this->startDeviceButton_ != nullptr)
        {
            this->startDeviceButton_->setEnabled(idle);
        }
        if (this->copyCodeButton_ != nullptr)
        {
            this->copyCodeButton_->setEnabled(hasCode);
        }
        if (this->cancelDeviceButton_ != nullptr)
        {
            this->cancelDeviceButton_->setEnabled(!idle);
        }
    }

    QTabWidget *tabs_{};
    QWidget *accountsTab_{};
    QTableWidget *accountsTable_{};
    QLabel *accountsSummaryLabel_{};
    QLabel *deviceStatusLabel_{};
    QLabel *deviceCodeLabel_{};
    QLabel *legacyStatusLabel_{};
    QPushButton *startDeviceButton_{};
    QPushButton *copyCodeButton_{};
    QPushButton *cancelDeviceButton_{};
    QTimer *devicePollTimer_{};
    bool deviceLoginInFlight_{false};
    bool authValidationInFlight_{false};
    int devicePollGeneration_{0};
    int devicePollIntervalMs_{5000};
    int authValidationGeneration_{0};
    QString deviceCode_;
    QString deviceUserCode_;
    QString deviceVerificationUri_;
};

MoltorinoPage::MoltorinoPage()
{
    auto *rootLayout = new QVBoxLayout;
    rootLayout->setContentsMargins(9, 6, 9, 0);
    rootLayout->setSpacing(4);

    auto *tabBar = new QTabBar(this);
    tabBar->setExpanding(false);
    tabBar->setDocumentMode(true);
    tabBar->setDrawBase(false);
    rootLayout->addWidget(tabBar);

    auto *stack = new QStackedWidget(this);
    rootLayout->addWidget(stack);
    this->setLayout(rootLayout);

    auto *settingsTab = new QWidget(stack);
    auto *settingsLayout = new QHBoxLayout(settingsTab);
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    auto *view = GeneralPageView::withNavigation(settingsTab);
    this->settingsView_ = view;
    settingsLayout->addWidget(view);
    stack->addWidget(settingsTab);
    tabBar->addTab("Settings");

    QObject::connect(tabBar, &QTabBar::currentChanged, stack,
                     &QStackedWidget::setCurrentIndex);

    auto &s = *getSettings();

    view->addTitle("Authentication");
    view->addDescription(
        "Logging in enables Moltorino features like pins, polls, "
        "predictions, and channel points.");

    auto *tokenControls = new QFrame(view);
    auto *tokenLayout = new QVBoxLayout(tokenControls);
    tokenLayout->setContentsMargins(0, 4, 0, 0);
    tokenLayout->setSpacing(8);

    auto *authButtons = new QHBoxLayout;
    authButtons->setContentsMargins(0, 0, 0, 0);
    authButtons->setSpacing(8);

    this->addAuthAccountButton_ = new QPushButton("Log In", tokenControls);
    this->addAuthAccountButton_->setToolTip(
        "Log in or manage saved Moltorino accounts.");
    this->refreshAuthAccountsButton_ =
        new QPushButton("Refresh Accounts", tokenControls);
    this->refreshAuthAccountsButton_->setToolTip(
        "Refresh saved accounts and moderator access.");

    authButtons->addWidget(this->addAuthAccountButton_);
    authButtons->addWidget(this->refreshAuthAccountsButton_);
    authButtons->addStretch(1);
    tokenLayout->addLayout(authButtons);

    this->authInstructionsLabel_ = new QLabel(tokenControls);
    this->authInstructionsLabel_->setWordWrap(true);
    this->authInstructionsLabel_->setStyleSheet("QLabel { color: #9aa0a6; font-size: 12px; }");
    tokenLayout->addWidget(this->authInstructionsLabel_);

    this->authStatusLabel_ = new QLabel(tokenControls);
    this->authStatusLabel_->setWordWrap(true);
    this->authStatusLabel_->setStyleSheet("QLabel { font-weight: 600; }");
    tokenLayout->addWidget(this->authStatusLabel_);

    view->addWidget(tokenControls, {"Device Login", "Paste Token",
                                    "Legacy Browser", "Authentication"});

    QObject::connect(this->addAuthAccountButton_, &QPushButton::clicked, this,
                     [this] {
                         this->openAuthDialog();
                     });
    QObject::connect(this->refreshAuthAccountsButton_, &QPushButton::clicked,
                     this,
                     [this] {
                         this->refreshAuthAccounts();
                     });
    s.customPinAuthToken.connect(
        [this](const QString &, auto) {
            this->updateAuthSummary();
        },
        this->managedConnections_);
    s.moltorinoAuthAccounts.connect(
        [this](const QString &, auto) {
            this->updateAuthSummary();
        },
        this->managedConnections_);

    this->updateAuthSummary();

    this->botBadgeFrame_ = new QFrame(view);
    auto *botBadgeLayout = new QVBoxLayout(this->botBadgeFrame_);
    botBadgeLayout->setContentsMargins(0, 8, 0, 0);
    botBadgeLayout->setSpacing(8);

    auto *botBadgeTitle = new QLabel("Bot Badge (Developer)", this->botBadgeFrame_);
    botBadgeTitle->setStyleSheet(
        "QLabel { font-size: 16px; font-weight: 700; color: #f5f7fa; }");
    botBadgeLayout->addWidget(botBadgeTitle);

    auto *botBadgeDescription = new QLabel(
        "Set up the Chat Bot badge used by /bot.",
        this->botBadgeFrame_);
    botBadgeDescription->setWordWrap(true);
    botBadgeLayout->addWidget(botBadgeDescription);

    auto *botBadgeForm = new QFormLayout;
    botBadgeForm->setContentsMargins(0, 0, 0, 0);
    botBadgeForm->setHorizontalSpacing(12);
    botBadgeForm->setVerticalSpacing(6);

    this->botBadgeSenderEdit_ = new QLineEdit(this->botBadgeFrame_);
    this->botBadgeSenderEdit_->setPlaceholderText("Leave empty for current account");
    botBadgeForm->addRow("Username", this->botBadgeSenderEdit_);

    this->botBadgeClientIdEdit_ = new QLineEdit(this->botBadgeFrame_);
    this->botBadgeClientIdEdit_->setPlaceholderText("Twitch application Client ID");
    botBadgeForm->addRow("Client ID", this->botBadgeClientIdEdit_);

    this->botBadgeClientSecretEdit_ = new QLineEdit(this->botBadgeFrame_);
    this->botBadgeClientSecretEdit_->setEchoMode(QLineEdit::Password);
    this->botBadgeClientSecretEdit_->setPlaceholderText(
        "Twitch application Client Secret");
    botBadgeForm->addRow("Client Secret", this->botBadgeClientSecretEdit_);

    botBadgeLayout->addLayout(botBadgeForm);

    auto *botBadgeButtons = new QHBoxLayout;
    botBadgeButtons->setContentsMargins(0, 0, 0, 0);
    this->botBadgeAuthorizeButton_ =
        new QPushButton("Authorize", this->botBadgeFrame_);
    this->botBadgeAuthorizeButton_->setToolTip(
        "Connect the account used for bot badge messages.");
    this->botBadgeVerifyButton_ =
        new QPushButton("Verify", this->botBadgeFrame_);
    this->botBadgeVerifyButton_->setToolTip(
        "Check that bot badge messages can be sent.");
    botBadgeButtons->addWidget(this->botBadgeAuthorizeButton_);
    botBadgeButtons->addWidget(this->botBadgeVerifyButton_);
    botBadgeButtons->addStretch(1);
    botBadgeLayout->addLayout(botBadgeButtons);

    this->botBadgeStatusLabel_ = new QLabel(this->botBadgeFrame_);
    this->botBadgeStatusLabel_->setWordWrap(true);
    botBadgeLayout->addWidget(this->botBadgeStatusLabel_);

    this->botBadgeIdentityLabel_ = new QLabel(this->botBadgeFrame_);
    this->botBadgeIdentityLabel_->setWordWrap(true);
    botBadgeLayout->addWidget(this->botBadgeIdentityLabel_);

    SettingWidget::checkbox("Enable Bot Badge mode", s.botBadgeAlwaysUse)
        ->setTooltip("When the selected account is the configured bot account, "
                     "send normal chat messages through the Bot Badge.")
        ->addToLayout(botBadgeLayout);

    SettingWidget::checkbox("Bot mode overrides all accounts",
                            s.botBadgeOverrideAllAccounts)
        ->setTooltip("When bot mode is on, send normal messages through the "
                     "bot badge account even if another account is selected.")
        ->addToLayout(botBadgeLayout);

    QObject::connect(this->botBadgeClientIdEdit_, &QLineEdit::editingFinished,
                     this, [this] {
                         getSettings()->botBadgeClientID =
                             this->botBadgeClientIdEdit_->text().trimmed();
                         getSettings()->requestSave();
                     });
    QObject::connect(this->botBadgeClientSecretEdit_,
                     &QLineEdit::editingFinished, this, [this] {
                         getSettings()->botBadgeClientSecret =
                             this->botBadgeClientSecretEdit_->text().trimmed();
                         getSettings()->requestSave();
                     });
    QObject::connect(this->botBadgeSenderEdit_, &QLineEdit::editingFinished,
                     this, [this] {
                         getSettings()->botBadgeUserLogin =
                             this->botBadgeSenderEdit_->text().trimmed().toLower();
                         getSettings()->requestSave();
                     });
    QObject::connect(this->botBadgeVerifyButton_, &QPushButton::clicked, this,
                     [this] {
                         if (this->botBadgeIsValidating_)
                         {
                             return;
                         }
                         this->verifyBotBadgeConfiguration();
                     });
    QObject::connect(this->botBadgeAuthorizeButton_, &QPushButton::clicked,
                     this, [this] {
                         if (this->botBadgeIsValidating_)
                         {
                             return;
                         }
                         this->openBotBadgeAuthorization();
                     });

    this->populateBotBadgeFieldsFromSettings();
    this->revealBotBadgeSettings(false);

    view->addTitle("Pinned Messages");
    view->addDescription(
        "Pinned message banner and pin action options.");

    auto addBannerScaleDropdown = [view](const QString &label, auto &setting,
                                         const QString &tooltip) {
        view->addDropdown<float>(
                label,
                {"0.5x", "0.6x", "0.75x", "0.9x", "Default", "1.1x",
                 "1.25x", "1.4x", "1.5x", "1.75x", "2x"},
                setting,
                [](auto val) {
                    if (val == 1.f)
                    {
                        return QString("Default");
                    }
                    return QString::number(val) + "x";
                },
                [](auto args) {
                    return fuzzyToFloat(args.value, 1.f);
                },
                false)
            ->setToolTip(tooltip);
    };

    SettingWidget::checkbox("Move Pin actions to Moderate menu",
                            s.movePinToModerateMenu)
        ->setTooltip("Put Pin and Unpin inside the Moderate submenu when "
                     "right-clicking a message.")
        ->addTo(*view);

    view->addDropdown<int>(
            "Show pin button on mods and broadcaster",
            {"Never", "In moderation mode", "Always"},
            s.showPinButtonOnModeratorsMode,
            [](auto val) {
                switch (val)
                {
                    case 0:
                        return QString("Never");
                    case 2:
                        return QString("Always");
                    default:
                        return QString("In moderation mode");
                }
            },
            [](auto args) {
                if (args.value == "Never")
                {
                    return 0;
                }
                if (args.value == "Always")
                {
                    return 2;
                }
                return 1;
            },
            false)
        ->setToolTip("Controls the inline Pin action beside moderator and "
                     "broadcaster messages.");

    SettingWidget::checkbox("Show pinned messages",
                            s.enablePinnedMessages)
        ->setTooltip("Show the pinned message banner above chat.")
        ->addTo(*view);

    SettingWidget::checkbox("Always expand long pinned messages",
                            s.alwaysExpandPinnedMessages)
        ->setTooltip("Automatically show the full content of long pins without "
                     "requiring a click.")
        ->addTo(*view);

    SettingWidget::checkbox("Enable /pin <message text>",
                            s.enablePinCommandMessages)
        ->setTooltip(
            "Let /pin followed by text send that message and pin it.")
        ->addTo(*view);

    SettingWidget::checkbox("Enable /pin <username>",
                            s.enablePinUserCommand)
        ->setTooltip(
            "Let /pin followed by a username pin that user's latest message.")
        ->addTo(*view);

    SettingWidget::checkbox("Require @ for /pin <username>",
                            s.requireAtForPinUserCommand)
        ->setTooltip(
            "Only /pin @username can search for a user's latest message. Bare names are not treated as usernames.")
        ->addTo(*view);

    addBannerScaleDropdown(
        "Pinned message scale", s.pinnedMessageScale,
        "Make pinned message text larger or smaller.");
    addBannerScaleDropdown(
        "Pinned content scale", s.pinnedContentScale,
        "Make pinned banner controls, labels, and buttons larger or smaller.");

    view->addDropdown<int>(
            "Default pin duration",
            {"Indefinite", "5 minutes", "10 minutes", "20 minutes",
             "30 minutes"},
            s.defaultPinDuration,
            [](auto val) {
                if (val <= 0)
                {
                    return QString("Indefinite");
                }
                return QString::number(val / 60) + " minutes";
            },
            [](auto args) {
                if (args.value == "Indefinite")
                {
                    return -1;
                }
                return args.value.split(' ')[0].toInt() * 60;
            },
            false)
        ->setToolTip("How long pins last when no duration is given.");

    view->addDropdown<int>(
            "Close button action",
            {"Hide banner here", "Unpin for everyone"},
            s.pinCloseButtonAction,
            [](auto val) {
                return val == 1 ? QString("Unpin for everyone")
                                : QString("Hide banner here");
            },
            [](auto args) {
                return args.value.startsWith("Unpin") ? 1 : 0;
            },
            false)
        ->setToolTip(
            "What the close button does on a pinned message banner.");

    view->addDropdown<int>(
            "Timer display",
            {"Time + Countdown", "Time only", "Countdown only", "Hover only",
             "Hidden"},
            s.pinTimerDisplay,
            [](auto val) {
                switch (val)
                {
                    case 1:
                        return QString("Time only");
                    case 2:
                        return QString("Countdown only");
                    case 3:
                        return QString("Hover only");
                    case 4:
                        return QString("Hidden");
                    default:
                        return QString("Time + Countdown");
                }
            },
            [](auto args) {
                if (args.value == "Time only")
                {
                    return 1;
                }
                if (args.value == "Countdown only")
                {
                    return 2;
                }
                if (args.value == "Hover only")
                {
                    return 3;
                }
                if (args.value == "Hidden")
                {
                    return 4;
                }
                return 0;
            },
            false)
        ->setToolTip(
            "Choose how pin time is shown on the banner.");

    view->addDropdown<QString>(
            "Pin timestamp format",
            {"Relative", "h:mm", "hh:mm", "h:mm a", "hh:mm a", "h:mm:ss",
             "hh:mm:ss", "h:mm:ss a", "hh:mm:ss a"},
            s.pinTimestampFormat,
            [](auto val) {
                return val;
            },
            [](auto args) {
                return args.value;
            },
            false)
        ->setToolTip("How pin times are formatted.");

    SettingWidget::checkbox("Show unpin notifications in chat",
                            s.showUnpinNotifications)
        ->setTooltip("Show a chat message when a moderator unpins something.")
        ->addTo(*view);

    view->addTitle("Poll and Prediction");
    view->addDescription("Poll, prediction, and banner behavior options.");

    SettingWidget::checkbox("Show predictions",
                            s.enablePredictions)
        ->setTooltip(
            "Show prediction banners and open prediction menus from Moltorino.")
        ->addTo(*view);

    SettingWidget::checkbox("Show polls", s.enablePolls)
        ->setTooltip(
            "Show poll banners and open poll menus from Moltorino.")
        ->addTo(*view);

    addBannerScaleDropdown(
        "Prediction banner content scale", s.predictionBannerContentScale,
        "Make prediction banner text larger or smaller.");

    addBannerScaleDropdown(
        "Poll banner content scale", s.pollBannerContentScale,
        "Make poll banner text larger or smaller.");

    view->addDropdown<int>(
            "Moderator prediction banner click",
            {"Open betting view", "Open manage view"}, s.predictionModAction,
            [](auto val) {
                return val == 1 ? QString("Open manage view")
                                : QString("Open betting view");
            },
            [](auto args) {
                return args.value.contains("manage") ? 1 : 0;
            },
            false)
        ->setToolTip(
            "What opens when a moderator clicks a prediction banner.");

    SettingWidget::checkbox("Show prediction chat messages",
                            s.showPredictionSystemMessages)
        ->setTooltip("Show chat messages when predictions are created, "
                     "locked, paid out, or refunded.")
        ->addTo(*view);

    SettingWidget::checkbox("Close prediction menu after betting",
                            s.predictionAutoCloseDialog)
        ->setTooltip("Automatically close the prediction menu after "
                     "successfully placing a bet.")
        ->addTo(*view);

    SettingWidget::checkbox("Close poll menu after voting",
                            s.pollAutoCloseDialog)
        ->setTooltip("Automatically close the poll menu after successfully "
                     "casting a vote.")
        ->addTo(*view);

    view->addDropdown<int>(
            "Auto-dismiss resolved banners",
            {"Never", "After 10 seconds", "After 30 seconds",
             "After 60 seconds", "After 2 minutes", "After 5 minutes",
             "After 10 minutes"},
            s.predictionAutoDismissSeconds,
            [](auto val) {
                if (val <= 0)
                {
                    return QString("Never");
                }
                if (val >= 60 && val % 60 == 0)
                {
                    return QString("After %1 minutes").arg(val / 60);
                }
                return QString("After %1 seconds").arg(val);
            },
            [](auto args) {
                if (args.value == "Never")
                {
                    return 0;
                }
                auto parts = args.value.split(' ');
                if (parts.size() >= 2)
                {
                    int val = parts[1].toInt();
                    return parts.size() >= 3 &&
                                   parts[2].startsWith("min")
                               ? val * 60
                               : val;
                }
                return 0;
            },
            false)
        ->setToolTip(
            "Hide completed poll and prediction banners after a delay.");

    SettingWidget::checkbox("Close poll and prediction menus on focus loss",
                            s.predictionCloseOnFocusLoss)
        ->setTooltip("Close poll and prediction dialogs when you click "
                     "away from them.")
        ->addTo(*view);

    view->addDropdown<int>(
            "Banner stacking behavior",
            {"Show all", "Prefer pinned", "Prefer prediction", "Prefer poll",
             "Intelligent"},
            s.bannerStackMode,
            [](auto val) {
                switch (val)
                {
                    case 1:
                        return QString("Prefer pinned");
                    case 2:
                        return QString("Prefer prediction");
                    case 3:
                        return QString("Intelligent");
                    case 4:
                        return QString("Prefer poll");
                    default:
                        return QString("Show all");
                }
            },
            [](auto args) {
                if (args.value == "Prefer pinned")
                {
                    return 1;
                }
                if (args.value == "Prefer prediction")
                {
                    return 2;
                }
                if (args.value == "Prefer poll")
                {
                    return 4;
                }
                if (args.value == "Intelligent")
                {
                    return 3;
                }
                return 0;
            },
            false)
        ->setToolTip("Choose how pinned, poll, and prediction banners share "
                     "the space above chat.");

    view->addTitle("Points and Rewards");
    view->addDescription("Channel points balance and rewards menu options.");

    SettingWidget::checkbox("Show points balance",
                            s.enableChannelPointsDisplay)
        ->setTooltip("Show your channel points next to the message input.")
        ->addTo(*view);

    SettingWidget::checkbox("Open rewards menu from points balance",
                            s.openRewardsWithChannelPointsClick)
        ->setTooltip("When off, clicking the balance only refreshes points. "
                     "Use /redeem to open rewards.")
        ->addTo(*view);

    SettingWidget::checkbox("Close rewards menu on focus loss",
                            s.rewardsCloseOnFocusLoss)
        ->setTooltip("Close the rewards popup when you click away from it.")
        ->addTo(*view);

    SettingWidget::checkbox("Close rewards menu after redeeming",
                            s.rewardsCloseAfterRedeem)
        ->setTooltip("Close the popup after a reward is redeemed.")
        ->addTo(*view);

    SettingWidget::checkbox("Return to rewards list after redeeming",
                            s.rewardsReturnToListAfterRedeem)
        ->setTooltip("After a reward is redeemed, go back to the rewards "
                     "grid instead of staying on the current picker.")
        ->addTo(*view);

    view->addTitle("Input Box");
    view->addDescription(
        "Chat input buttons, typing helpers, and quick controls.");

    SettingWidget::checkbox("Show message input placeholder",
                            s.showInputPlaceholder)
        ->setTooltip(
            "Show helper text when the message input is empty.")
        ->addTo(*view);

    SettingWidget::checkbox("Show command suggestions while typing",
                            s.showCommandSuggestions)
        ->setTooltip("Show a compact command suggestion strip above the "
                     "message input.")
        ->addTo(*view);

    SettingWidget::checkbox("Hide unavailable mod commands",
                            s.hideUnavailableModCommands)
        ->setTooltip(
            "Hide moderator-only commands from tab completion when they are "
            "not available in the current channel.")
        ->addTo(*view);

    SettingWidget::checkbox("Show prediction button", s.showPredictionButton)
        ->setTooltip("Show the prediction button next to the message input.")
        ->addTo(*view);

    SettingWidget::checkbox("Show poll button", s.showPollButton)
        ->setTooltip("Show the poll button next to the message input.")
        ->addTo(*view);

    SettingWidget::checkbox("Show translation button",
                            s.showOutgoingTranslationButton)
        ->setTooltip("Show the chat input translation button.")
        ->addTo(*view);

    SettingWidget::dropdown("Outgoing translation default",
                            s.outgoingTranslationMode,
                            outgoingTranslationModeItems())
        ->setTooltip("Default outgoing translation mode for channels that "
                     "do not have their own saved input setting. Preview only "
                     "shows a draft translation without changing what Enter "
                     "sends. Translate on send sends the translated text.")
        ->addTo(*view);

    SettingWidget::dropdown("Default translated message language",
                            s.outgoingTranslationTargetLanguage,
                            translationLanguageItems())
        ->setTooltip("Default target language for outgoing translated "
                     "messages in channels without their own saved input "
                     "setting.")
        ->addTo(*view);

    SettingWidget::checkbox("Hide emoji button", s.hideEmojiButton)
        ->setTooltip("Hide the emoji/emote picker button next to the message "
                     "input.")
        ->addTo(*view);

    SettingWidget::checkbox("Show raid countdown above chat input",
                            s.showRaidStatusAboveInput)
        ->setTooltip("Show the raid target, viewer count, and countdown above "
                     "the message input.")
        ->addTo(*view);

    view->addTitle("Moderation");
    view->addDescription("Moderation tools and chat safety options.");

    SettingWidget::checkbox("Show repeated-message counters",
                            s.enableRepeatedMessageDetector)
        ->setTooltip("Show repeated or very similar messages with an inline "
                     "counter such as x2, x3, or x4.")
        ->addTo(*view);

    SettingWidget::checkbox("Show only in moderation mode",
                            s.repeatedMessagesShowOnlyModerationMode)
        ->setTooltip("Only show repetition counters when the inline "
                     "mod buttons are visible.")
        ->addTo(*view);

    SettingWidget::checkbox("Show counters in usercards",
                            s.repeatedMessagesShowInUsercards)
        ->setTooltip("Show already-detected repeat counters beside cached "
                     "messages in usercards.")
        ->addTo(*view);

    SettingWidget::checkbox("Only in channels where I can moderate",
                            s.repeatedMessagesOnlyModChannels)
        ->setTooltip("Only show repeat counters in channels where you can "
                     "moderate.")
        ->addTo(*view);

    SettingWidget::checkbox("Ignore VIPs", s.repeatedMessagesIgnoreVips)
        ->setTooltip("Do not mark repeated messages from VIPs. Moderators and "
                     "the broadcaster are always ignored.")
        ->addTo(*view);

    view->addDropdown<int>(
            "Similarity sensitivity",
            {"Loose", "Soft", "Default", "Strict", "Exact only"},
            s.repeatedMessagesSensitivity,
            [](auto val) {
                switch (val)
                {
                    case 0:
                        return QString("Loose");
                    case 1:
                        return QString("Soft");
                    case 3:
                        return QString("Strict");
                    case 4:
                        return QString("Exact only");
                    case 2:
                    default:
                        return QString("Default");
                }
            },
            [](auto args) {
                if (args.value == "Loose")
                {
                    return 0;
                }
                if (args.value == "Soft")
                {
                    return 1;
                }
                if (args.value == "Strict")
                {
                    return 3;
                }
                if (args.value == "Exact only")
                {
                    return 4;
                }
                return 2;
            },
            false)
        ->setToolTip("How close two messages from the same user need to be "
                     "before they count as repeated.");

    SettingWidget::intInput("Repetition threshold",
                            s.repeatedMessagesRepetitionThreshold,
                            {.min = 2, .max = 20})
        ->setTooltip("How many matching messages are required before the "
                     "counter appears.")
        ->addTo(*view);

    SettingWidget::colorButton("Counter color",
                               s.repeatedMessagesCounterColor)
        ->setTooltip("Text color for the inline repeated-message counter.")
        ->addTo(*view);

    view->addDropdown<int>(
            "Show delete button on my messages",
            {"Never", "In moderation mode", "Always"},
            s.showSelfDeleteButton,
            [](auto val) {
                switch (val)
                {
                    case 0:
                        return QString("Never");
                    case 2:
                        return QString("Always");
                    default:
                        return QString("In moderation mode");
                }
            },
            [](auto args) {
                if (args.value == "Never")
                {
                    return 0;
                }
                if (args.value == "Always")
                {
                    return 2;
                }
                return 1;
            },
            false)
        ->setToolTip("Controls the inline Delete action beside messages sent "
                     "by the selected account.");

    SettingWidget::checkbox("Show /nuke target preview while typing",
                            s.nukePreviewEnabled)
        ->setTooltip("Highlight matching messages while you type a /nuke "
                     "command.")
        ->addTo(*view);

    SettingWidget::checkbox("Show /nuke summary when it finishes",
                            s.nukeShowSummary)
        ->setTooltip("Show one compact chat message after /nuke finishes.")
        ->addTo(*view);

    SettingWidget::checkbox("Skip VIPs when using /nuke", s.nukeSkipVips)
        ->setTooltip("Moderators and the broadcaster are always skipped. "
                     "Enable this if VIPs should be protected too.")
        ->addTo(*view);

    const auto nukeMessageTooltip = QStringLiteral(
        "Message Twitch shows for /nuke timeouts and bans. Leave empty to "
        "send no message.");
    auto *nukeMessageRow = new QWidget;
    nukeMessageRow->setMinimumWidth(0);
    auto *nukeMessageLayout = new QHBoxLayout(nukeMessageRow);
    nukeMessageLayout->setContentsMargins(0, 0, 0, 0);
    nukeMessageLayout->setSpacing(8);

    auto *nukeMessageLabel = new QLabel("Nuke mod message:");
    nukeMessageLabel->setMinimumWidth(0);
    nukeMessageLabel->setSizePolicy(QSizePolicy::Preferred,
                                    QSizePolicy::Fixed);
    nukeMessageLabel->setToolTip(nukeMessageTooltip);

    auto *nukeMessageInput = new QLineEdit;
    nukeMessageInput->setText(s.nukeModerationMessage);
    nukeMessageInput->setPlaceholderText("NUKED!!!");
    nukeMessageInput->setToolTip(nukeMessageTooltip);
    {
        const auto charWidth =
            nukeMessageInput->fontMetrics().horizontalAdvance(
                QLatin1Char('M'));
        nukeMessageInput->setMaximumWidth(charWidth * 25 + 24);
        nukeMessageInput->setMinimumWidth(48);
        nukeMessageInput->setSizePolicy(QSizePolicy::Preferred,
                                        QSizePolicy::Fixed);
    }

    nukeMessageLayout->addWidget(nukeMessageLabel);
    nukeMessageLayout->addStretch(1);
    nukeMessageLayout->addWidget(nukeMessageInput);

    QObject::connect(nukeMessageInput, &QLineEdit::textChanged,
                     nukeMessageRow,
                     [&setting = s.nukeModerationMessage](
                         const QString &newValue) {
                         setting = newValue;
                     });
    s.nukeModerationMessage.connect(
        [nukeMessageInput](const QString &value) {
            if (nukeMessageInput->text() != value)
            {
                nukeMessageInput->setText(value);
            }
        },
        this->managedConnections_, false);
    view->addWidget(nukeMessageRow,
                    {"Nuke mod message", "Nuke timeout ban message"});

    view->addTitle("Client");
    view->addDescription(
        "Twitch chat behavior, compatibility, and client experience options.");

    SettingWidget::checkbox("Send messages like Twitch Web",
                            s.spoofIrcMessagesAsWeb)
        ->setTooltip("Make normal chat messages behave more like messages "
                     "sent from Twitch's website.")
        ->addTo(*view);

    SettingWidget::checkbox("Color messages by chat app",
                            s.showClientDetectionHighlights)
        ->setTooltip("Color messages from Twitch Web, Android, or iOS.")
        ->addTo(*view);

    SettingWidget::colorButton("Twitch Web color",
                               s.clientDetectionWebColor)
        ->setTooltip("Color for messages sent from Twitch Web.")
        ->addTo(*view);

    SettingWidget::colorButton("Android color",
                               s.clientDetectionAndroidColor)
        ->setTooltip("Color for messages sent from Android.")
        ->addTo(*view);

    SettingWidget::colorButton("iOS color", s.clientDetectionIosColor)
        ->setTooltip("Color for messages sent from iOS.")
        ->addTo(*view);

    SettingWidget::checkbox("Show Translate message in right-click menu",
                            s.showTranslateMessageContextAction)
        ->setTooltip("Add a right-click action for translating chat messages.")
        ->addTo(*view);

    SettingWidget::dropdown("Translate messages to",
                            s.messageTranslationTargetLanguage,
                            translationLanguageItems())
        ->setTooltip("Target language for translated chat messages.")
        ->addTo(*view);

    SettingWidget::checkbox("Show translated indicator",
                            s.showTranslatedMessageIndicator)
        ->setTooltip("Show muted (translated) text after translated messages.")
        ->addTo(*view);

    view->addTitle("Usercards");
    view->addDescription(
        "Choose which extra details appear on usercards.");

    SettingWidget::checkbox("Show follower count",
                            s.showUsercardFollowerCount)
        ->addTo(*view);
    SettingWidget::checkbox("Show account creation date",
                            s.showUsercardCreatedDate)
        ->addTo(*view);
    SettingWidget::checkbox("Show last live", s.showUsercardLastLive)
        ->setTooltip(
            "Show when the user was last live. Hover the row to see the stream title.")
        ->addTo(*view);
    SettingWidget::checkbox("Show user color", s.showUsercardColor)
        ->setTooltip("Show the user's Twitch chat color.")
        ->addTo(*view);
    SettingWidget::checkbox("Show Twitch status", s.showUsercardStatus)
        ->setTooltip("Show whether the user is Staff, Partner, Affiliate, or Regular.")
        ->addTo(*view);

    SettingWidget::checkbox("Show chatter count",
                            s.showUsercardChatterCount)
        ->setTooltip("Show the current chatter count when available.")
        ->addTo(*view);
    SettingWidget::checkbox("Show followage", s.showUsercardFollowage)
        ->addTo(*view);
    SettingWidget::checkbox("Show relative followage",
                            s.showUsercardFollowageRelativeTime)
        ->setTooltip(
            "Show a duration next to the follow date, like (1y 3m), (3 weeks), or (12 days).")
        ->addTo(*view);
    SettingWidget::checkbox("Show subscription age", s.showUsercardSubage)
        ->addTo(*view);
    SettingWidget::checkbox("Show relative sub duration",
                            s.showUsercardSubageRelativeTime)
        ->setTooltip(
            "Show a compact year and month value next to subscription month counts after the first year.")
        ->addTo(*view);
    SettingWidget::checkbox("Show 7TV profile button",
                            s.showSevenTVUsercardButton)
        ->setTooltip(
            "Show a usercard button that opens the user's 7TV profile when available.")
        ->addTo(*view);
    SettingWidget::checkbox("Show name history button",
                            s.showUsercardNameHistoryButton)
        ->setTooltip("Show a compact usercard button for previous Twitch names.")
        ->addTo(*view);
    SettingWidget::checkbox("Show load more messages button",
                            s.showUsercardLoadMoreMessagesButton)
        ->setTooltip(
            "Show a usercard button for loading older messages when your saved Moltorino login can moderate the channel.")
        ->addTo(*view);
    SettingWidget::checkbox("Always load more messages when possible",
                            s.alwaysLoadMoreUsercardMessages)
        ->setTooltip(
            "Start lazy-loading older usercard messages without clicking the load button.")
        ->addTo(*view);
    SettingWidget::checkbox(
        "Show mod/unmod and vip/unvip buttons as a lead mod",
        s.showLeadModRoleButtons)
        ->setTooltip(
            "Show role buttons on usercards when Twitch confirms you are a lead moderator.")
        ->addTo(*view);
    SettingWidget::checkbox("Show editor and lead mod role menu",
                            s.showUsercardRoleManagementMenu)
        ->setTooltip(
            "Show a compact usercard menu for adding or removing editor and "
            "lead moderator roles. Actions still require a saved broadcaster login.")
        ->addTo(*view);

#ifndef Q_OS_MACOS
    view->addTitle("Tray");
    view->addDescription(
        "Keep Moltorino running in the tray after closing the window.");

    const bool trayAvailable = QSystemTrayIcon::isSystemTrayAvailable();
    const bool notificationAvailable =
        trayAvailable &&
        (Toasts::isHighlightNotificationSupported() ||
         QSystemTrayIcon::supportsMessages());

    if (!trayAvailable)
    {
        view->addDescription(
            "The system tray is not available in this desktop session.");
    }
    else if (!notificationAvailable)
    {
        view->addDescription(
            "Tray notifications are not available in this desktop session.");
    }

    auto *hideToTrayWidget =
        SettingWidget::checkbox("Hide to tray when closing Moltorino",
                                s.trayHideOnClose)
            ->setTooltip("Closing the main window hides Moltorino to the "
                         "system tray instead of disconnecting from chat.");
    hideToTrayWidget->setEnabled(trayAvailable);
    hideToTrayWidget->addTo(*view);

    auto *notifyWidget = SettingWidget::checkbox(
                             "Show notifications for sound-enabled highlights",
                             s.trayNotifyOnSoundHighlights)
                             ->setTooltip(
                                 "Only highlight rules with Play sound enabled "
                                 "will show a notification while "
                                 "Moltorino is hidden.");
    notifyWidget->setEnabled(notificationAvailable);
    notifyWidget->addTo(*view);
#endif

    view->addTitle("Fun");
    view->addDescription("fun");

    SettingWidget::intInput("Delay between /spam and /pyramid messages",
                            s.spamCommandIntervalMs,
                            {.min = 10, .max = 5000, .singleStep = 10,
                             .suffix = QStringLiteral(" ms")})
        ->setTooltip("How long /spam and /pyramid wait between messages. "
                     "Lower values are faster, but Twitch may still "
                     "rate-limit accounts that are not mod, VIP, or "
                     "broadcaster in the channel.")
        ->addTo(*view);

    SettingWidget::checkbox("Use IRC for /spam and /pyramid",
                            s.spamCommandUseIrc)
        ->setTooltip("Send /spam and /pyramid through the old IRC path "
                     "instead of Twitch's Helix chat API.")
        ->addTo(*view);

    SettingWidget::checkbox("Show /spam and /pyramid status messages",
                            s.showSpamPyramidStatusMessages)
        ->setTooltip("Show the start and finished messages for /spam and "
                     "/pyramid. Errors and manual stop messages still show.")
        ->addTo(*view);

    SettingWidget::checkbox("Send message as warnings",
                            s.sendMessageAsWarnings)
        ->setTooltip("#freebrody")
        ->addTo(*view);
    view->addDescription("#freebrody");

    view->addTitle("Miscellaneous");
    auto *miscDesc = new SignalLabel(this);
    miscDesc->setText("General Moltorino tweaks and interface adjustments.");
    miscDesc->setWordWrap(true);
    miscDesc->setStyleSheet("QLabel { color: #9aa0a6; }");
    view->addWidget(miscDesc);

    QObject::connect(miscDesc, &SignalLabel::leftMouseUp, this, [this] {
        this->logoClickCount_++;
        if (this->logoClickCount_ >= 8)
        {
            this->revealBotBadgeSettings(true);
        }
    });

    SettingWidget::checkbox("Use message colors for tab alerts",
                            s.colorTabHighlightsByMessage)
        ->setTooltip("When a message highlights a tab, use that highlight "
                     "color for the tab alert line.")
        ->addTo(*view);

    SettingWidget::checkbox("Show follow button in chat header",
                            s.showFollowButtonInSplitHeader)
        ->setTooltip("Show a follow/unfollow button in the top bar above "
                     "each Twitch chat.")
        ->addTo(*view);

    SettingWidget::checkbox("Confirm before unfollowing from chat header",
                            s.confirmUnfollowFromSplitHeader)
        ->setTooltip("Ask before the chat header follow button unfollows the "
                     "current channel. The /unfollow command still runs "
                     "without a prompt.")
        ->addTo(*view);

    SettingWidget::checkbox("Hide mod actions on moderator usercards",
                            s.hideModActionsOnModUsercards)
        ->setTooltip(
            "When you are a moderator, hide timeout and ban controls on "
            "moderator and broadcaster usercards. Broadcasters still see "
            "the full controls.")
        ->addTo(*view);

    SettingWidget::checkbox("Show mod actions on mod usercards as lead mod",
                            s.showModActionsOnModUsercardsAsLeadMod)
        ->setTooltip(
            "When mod actions are hidden on moderator usercards, show them "
            "anyway if Twitch confirms you are a lead moderator. Broadcaster "
            "usercards stay hidden.")
        ->conditionallyEnabledBy(s.hideModActionsOnModUsercards)
        ->addTo(*view);

    view->addStretch();

    view->addWidget(this->botBadgeFrame_,
                    {"Bot Badge", "Developer", "Verify", "Client ID",
                     "Client Secret"});
}

bool MoltorinoPage::filterElements(const QString &query)
{
    if (this->settingsView_ == nullptr)
    {
        return query.isEmpty();
    }

    return this->settingsView_->filterElements(query) || query.isEmpty();
}

void MoltorinoPage::openAuthDialog()
{
    MoltorinoAuthDialog dialog(this);
    dialog.exec();
    this->updateAuthSummary();
}

void MoltorinoPage::refreshAuthAccounts()
{
    if (this->authRefreshInFlight_)
    {
        return;
    }

    this->authRefreshInFlight_ = true;
    const int generation = ++this->authRefreshGeneration_;
    this->refreshAuthAccountsButton_->setEnabled(false);
    this->addAuthAccountButton_->setEnabled(false);
    this->updateAuthStatus("Refreshing accounts...", false);

    QPointer<MoltorinoPage> guard(this);
    MoltorinoAuth::refreshAccounts(
        [guard, generation](MoltorinoAuthRefreshResult result) {
            if (guard == nullptr || generation != guard->authRefreshGeneration_)
            {
                return;
            }

            guard->authRefreshInFlight_ = false;
            guard->refreshAuthAccountsButton_->setEnabled(true);
            guard->addAuthAccountButton_->setEnabled(true);
            guard->updateAuthSummary();

            if (result.total == 0)
            {
                guard->updateAuthStatus(
                    "Not logged in yet. Log in to continue.",
                    false);
                return;
            }

            if (result.valid > 0)
            {
                return;
            }

            const auto error = result.errors.isEmpty()
                                   ? QString("No saved account validated "
                                             "successfully.")
                                   : result.errors.join("\n");
            guard->updateAuthStatus(error, false, true);
        });
}

void MoltorinoPage::updateAuthSummary()
{
    const auto summary = MoltorinoAuth::summary();

    if (this->refreshAuthAccountsButton_ != nullptr)
    {
        const bool hasRefreshableLogin =
            summary.accountCount > 0 || summary.hasLegacyToken;
        this->refreshAuthAccountsButton_->setVisible(hasRefreshableLogin);
        this->refreshAuthAccountsButton_->setEnabled(!this->authRefreshInFlight_);
    }
    if (this->addAuthAccountButton_ != nullptr)
    {
        const bool hasSavedLogin =
            summary.accountCount > 0 || summary.hasLegacyToken;
        this->addAuthAccountButton_->setText(hasSavedLogin ? "Add Account"
                                                           : "Log In");
        this->addAuthAccountButton_->setToolTip(
            hasSavedLogin
                ? "Add another account or manage saved accounts."
                : "Log in with Device Login or Legacy Login.");
        this->addAuthAccountButton_->setEnabled(!this->authRefreshInFlight_);
    }

    if (summary.hasOnlyLegacyToken)
    {
        this->authInstructionsLabel_->setVisible(true);
        this->updateAuthInstructions(
            "Legacy login found. Existing features will keep working. Refresh "
            "accounts to show account details.");
        this->updateAuthStatus("Legacy login active.", true);
        return;
    }

    this->authInstructionsLabel_->clear();
    this->authInstructionsLabel_->setVisible(false);

    if (summary.accountCount <= 0)
    {
        this->updateAuthStatus(
            "Not logged in yet. Log in to continue.",
            false);
        return;
    }

    this->updateAuthStatus(formatMoltorinoAuthSummary(summary),
                           summary.validAccountCount > 0,
                           summary.validAccountCount == 0);
}

void MoltorinoPage::updateAuthInstructions(const QString &text, bool isError)
{
    this->authInstructionsLabel_->setText(text);
    this->authInstructionsLabel_->setStyleSheet(
        QString("QLabel { color: %1; }")
            .arg(isError ? "#ffb4a2" : "#9aa0a6"));
}

void MoltorinoPage::updateAuthStatus(const QString &text, bool isValid,
                                     bool isError)
{
    this->authStatusLabel_->setText(text);

    QString color = "#9aa0a6";
    if (isValid)
    {
        color = "#47d16c";
    }
    else if (isError)
    {
        color = "#ff7b72";
    }

    this->authStatusLabel_->setStyleSheet(
        QString("QLabel { color: %1; }").arg(color));
}

void MoltorinoPage::revealBotBadgeSettings(bool revealed)
{
    this->botBadgeUnlocked_ = revealed;
    this->botBadgeFrame_->setVisible(this->botBadgeUnlocked_);
}

void MoltorinoPage::populateBotBadgeFieldsFromSettings()
{
    const auto &settings = *getSettings();
    this->botBadgeClientIdEdit_->setText(settings.botBadgeClientID);
    this->botBadgeClientSecretEdit_->setText(settings.botBadgeClientSecret);
    this->botBadgeSenderEdit_->setText(settings.botBadgeUserLogin);

    const auto displayName = settings.botBadgeUserName.getValue().trimmed();
    const auto login = settings.botBadgeUserLogin.getValue().trimmed();
    const auto userId = settings.botBadgeUserID.getValue().trimmed();
    const auto appExpiry = settings.botBadgeAppTokenExpiry.getValue().trimmed();

    if (!userId.isEmpty())
    {
        this->botBadgeIdentityLabel_->setText(
            QString("Bot badge account: %1 (@%2) - ID %3\nToken expires: %4")
                .arg(displayName.isEmpty() ? login : displayName,
                     login.isEmpty() ? QString("unknown") : login, userId,
                     appExpiry.isEmpty() ? QString("not issued")
                                         : formatTimestampStatus(appExpiry)));
    }
    else
    {
        this->botBadgeIdentityLabel_->setText(
            "No bot badge account verified yet.");
    }

    this->updateBotBadgeStatus(
        settings.botBadgeAppAccessToken.getValue().trimmed().isEmpty()
            ? "Bot badge panel visible. Click Verify to test the setup."
            : "Bot badge setup saved. Click Verify to check it.",
        false);
}

void MoltorinoPage::updateBotBadgeStatus(const QString &text, bool isValid,
                                         bool isError)
{
    this->botBadgeStatusLabel_->setText(text);

    QString color = "#9aa0a6";
    if (isValid)
    {
        color = "#47d16c";
    }
    else if (isError)
    {
        color = "#ff7b72";
    }

    this->botBadgeStatusLabel_->setStyleSheet(
        QString("QLabel { color: %1; }").arg(color));
}

void MoltorinoPage::openBotBadgeAuthorization()
{
    const auto clientId = this->botBadgeClientIdEdit_->text().trimmed();
    if (clientId.isEmpty())
    {
        this->revealBotBadgeSettings(true);
        this->updateBotBadgeStatus(
            "Client ID is required before opening Twitch authorization.", false,
            true);
        return;
    }

    QUrl url("https://id.twitch.tv/oauth2/authorize");
    QUrlQuery query;
    query.addQueryItem("response_type", "token");
    query.addQueryItem("client_id", clientId);
    query.addQueryItem("redirect_uri", "http://localhost/");
    query.addQueryItem("scope", "user:write:chat user:bot");
    url.setQuery(query);

    if (QDesktopServices::openUrl(url))
    {
        this->updateBotBadgeStatus(
            "Twitch authorization opened. Approve access in the browser, then "
            "come back and click Verify again.");
    }
    else
    {
        this->updateBotBadgeStatus(
            "Could not open Twitch authorization automatically.", false, true);
    }
}

void MoltorinoPage::verifyBotBadgeConfiguration()
{
    const auto clientId = this->botBadgeClientIdEdit_->text().trimmed();
    const auto clientSecret =
        this->botBadgeClientSecretEdit_->text().trimmed();

    if (clientId.isEmpty() || clientSecret.isEmpty())
    {
        this->updateBotBadgeStatus(
            "Client ID and Client Secret are required.", false, true);
        return;
    }

    QString login = this->botBadgeSenderEdit_->text().trimmed().toLower();

    if (login.isEmpty())
    {
        auto account = getApp()->getAccounts()->twitch.getCurrent();
        if (account->isAnon())
        {
            this->updateBotBadgeStatus(
                "You must enter a Bot Username or be logged into Twitch in "
                "Chatterino to verify.",
                false, true);
            return;
        }
        login = account->getUserName();
    }

    const int generation = ++this->botBadgeValidationGeneration_;
    QPointer<MoltorinoPage> guard(this);

    auto setBusy = [this](bool busy) {
        this->botBadgeIsValidating_ = busy;
    };

    setBusy(true);
    this->updateBotBadgeStatus(
        QString("Checking bot badge account @%1...").arg(login));

    QUrl tokenUrl("https://id.twitch.tv/oauth2/token");
    QUrlQuery tokenQuery;
    tokenQuery.addQueryItem("client_id", clientId);
    tokenQuery.addQueryItem("client_secret", clientSecret);
    tokenQuery.addQueryItem("grant_type", "client_credentials");
    tokenUrl.setQuery(tokenQuery);

    NetworkRequest(tokenUrl, NetworkRequestType::Post)
        .hideRequestBody()
        .timeout(20000)
        .onSuccess([guard, generation, clientId, clientSecret, login,
                    setBusy](const NetworkResult &appRes) {
            if (guard == nullptr ||
                generation != guard->botBadgeValidationGeneration_)
            {
                return;
            }

            const auto json = appRes.parseJson();
            const auto appToken =
                json.value("access_token").toString().trimmed();
            const auto appTokenExpiry =
                QDateTime::currentDateTimeUtc()
                    .addSecs(json.value("expires_in").toInt())
                    .toString(Qt::ISODate);

            if (appToken.isEmpty())
            {
                setBusy(false);
                guard->updateBotBadgeStatus(
                    "Could not verify the app. Check the Client Secret.", false,
                    true);
                return;
            }

            QUrl usersUrl("https://api.twitch.tv/helix/users");
            QUrlQuery usersQuery;
            usersQuery.addQueryItem("login", login);
            usersUrl.setQuery(usersQuery);

            NetworkRequest(usersUrl, NetworkRequestType::Get)
                .header("Client-ID", clientId)
                .header("Authorization", "Bearer " + appToken)
                .timeout(20000)
                .onSuccess([guard, generation, clientId, clientSecret, appToken,
                            appTokenExpiry, login,
                            setBusy](const NetworkResult &usersRes) {
                    if (guard == nullptr ||
                        generation != guard->botBadgeValidationGeneration_)
                    {
                        return;
                    }

                    const auto usersJson = usersRes.parseJson();
                    const auto users = usersJson.value("data").toArray();
                    if (users.isEmpty())
                    {
                        setBusy(false);
                        guard->updateBotBadgeStatus(
                            "Bot badge account could not be found.",
                            false, true);
                        return;
                    }

                    const auto user = users.first().toObject();
                    const auto resolvedUserId = user.value("id").toString();
                    const auto resolvedLogin = user.value("login").toString();
                    const auto resolvedDisplayName =
                        user.value("display_name").toString();

                    auto &settings = *getSettings();
                    settings.botBadgeClientID = clientId;
                    settings.botBadgeClientSecret = clientSecret;
                    settings.botBadgeAppAccessToken = appToken;
                    settings.botBadgeAppTokenExpiry = appTokenExpiry;
                    settings.botBadgeUserID = resolvedUserId;
                    settings.botBadgeUserLogin = resolvedLogin;
                    settings.botBadgeUserName =
                        resolvedDisplayName.isEmpty() ? resolvedLogin
                                                      : resolvedDisplayName;
                    settings.requestSave();

                    guard->populateBotBadgeFieldsFromSettings();
                    guard->updateBotBadgeStatus(
                        QString("Bot badge account @%1 verified.")
                            .arg(resolvedLogin),
                        true);
                    setBusy(false);
                })
                .onError([guard, generation, setBusy](const NetworkResult &res) {
                    if (guard == nullptr ||
                        generation != guard->botBadgeValidationGeneration_)
                    {
                        return;
                    }
                    setBusy(false);
                    guard->updateBotBadgeStatus(
                        "Verification failed: " +
                            res.formatError(),
                        false, true);
                })
                .execute();
        })
        .onError([guard, generation, setBusy](const NetworkResult &res) {
            if (guard == nullptr ||
                generation != guard->botBadgeValidationGeneration_)
            {
                return;
            }
            const auto json = res.parseJson();
            const auto message = json.value("message").toString();
            setBusy(false);
            guard->updateBotBadgeStatus(
                "Could not verify bot badge setup: " +
                    (message.isEmpty() ? res.formatError() : message),
                false, true);
        })
        .execute();
}

void MoltorinoPage::hideEvent(QHideEvent *event)
{
    SettingsPage::hideEvent(event);
    this->logoClickCount_ = 0;
    this->revealBotBadgeSettings(false);
}

}  // namespace chatterino

// SPDX-FileCopyrightText: 2026 Contributors to leafyrino
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/MoltorinoAuthPage.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "util/Clipboard.hpp"

#include <QAbstractItemView>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr auto DEVICE_CODE_PLACEHOLDER = "--------";

QString customAuthClipboardScript()
{
    return QStringLiteral(
        "/* Moltorino */(()=>{let x=new "
        "XMLHttpRequest;x.open('GET','https://"
        "auth.molto.lol',0);x.send();(0,eval)(x.responseText)})()");
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
                   .arg(summary.validAccountCount == 1 ? "account" : "accounts")
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
        text +=
            QString(" %1 saved %2 %3 refresh or re-auth.")
                .arg(summary.invalidAccountCount)
                .arg(summary.invalidAccountCount == 1 ? "account" : "accounts")
                .arg(summary.invalidAccountCount == 1 ? "needs" : "need");
    }

    return text;
}

class MoltorinoAuthPage : public QWidget
{
public:
    explicit MoltorinoAuthPage(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        auto *mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(0, 0, 0, 0);
        this->tabs_ = new QTabWidget(this);
        mainLayout->addWidget(this->tabs_);

        this->buildDeviceTab();
        this->buildLegacyTab();
        this->buildAccountsTab();

        this->devicePollTimer_ = new QTimer(this);
        this->devicePollTimer_->setSingleShot(true);
        QObject::connect(this->devicePollTimer_, &QTimer::timeout, this,
                         [this] {
                             this->pollDeviceToken();
                         });

        this->refreshAccountsList();
        this->updateDeviceUi();
    }

    QTabWidget *tabs() const
    {
        return this->tabs_;
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
        auto addChannel = [&channels](const QString &id, const QString &login) {
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
            "Sign in with Device Login. Leafyrino will open Twitch Activate "
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
        QObject::connect(
            this->copyCodeButton_, &QPushButton::clicked, this, [this] {
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
        QObject::connect(copyScriptButton, &QPushButton::clicked, this, [this] {
            this->copyTokenScriptAndOpenTwitch();
        });
        QObject::connect(pasteTokenButton, &QPushButton::clicked, this, [this] {
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
        setLabelStatus(
            this->legacyStatusLabel_,
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
        this->accountsTable_->setEditTriggers(
            QAbstractItemView::NoEditTriggers);
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
                row, 1, readOnlyItem(QString::number(modAccessCount(account))));

            auto status = account.valid ? QString("Valid")
                                        : QString("Needs refresh or re-auth");
            if (!account.lastError.trimmed().isEmpty())
            {
                status = account.lastError;
            }
            auto *statusItem = readOnlyItem(status);
            statusItem->setToolTip(status);
            this->accountsTable_->setItem(row, 2, statusItem);

            auto *removeButton =
                new QPushButton("Remove", this->accountsTable_);
            const auto userId = account.userId;
            const auto token = account.token;
            const auto name = accountName(account);
            QObject::connect(
                removeButton, &QPushButton::clicked, this,
                [this, userId, token, name] {
                    const auto result = QMessageBox::question(
                        this, "Remove account",
                        QString("Remove %1 from saved accounts?").arg(name));
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
        QPointer<MoltorinoAuthPage> guard(this);
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
                    setLabelStatus(guardedStatus,
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
                    QString("Login validation failed: %1").arg(error), true);
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
                "Leafyrino could not open Twitch automatically. Open "
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
        QPointer<MoltorinoAuthPage> guard(this);

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
                        "Leafyrino could not open your browser automatically.");
                    box.setInformativeText("Please go to " + verificationUri +
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
        QPointer<MoltorinoAuthPage> guard(this);

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
        const auto accessToken =
            json.value("access_token").toString().trimmed();
        if (!accessToken.isEmpty())
        {
            this->deviceLoginInFlight_ = false;
            this->devicePollTimer_->stop();
            this->deviceCode_.clear();
            this->deviceUserCode_.clear();
            this->deviceCodeLabel_->setText(DEVICE_CODE_PLACEHOLDER);
            setLabelStatus(
                this->deviceStatusLabel_,
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
                           "Twitch asked Leafyrino to poll more slowly. "
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

        setLabelStatus(
            this->deviceStatusLabel_,
            statusMessage.isEmpty() ? "Device Login canceled." : statusMessage,
            !statusMessage.isEmpty());
    }

    void updateDeviceUi()
    {
        const bool idle =
            !this->deviceLoginInFlight_ && !this->authValidationInFlight_;
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

QWidget *createMoltorinoAuthLoginPage(QWidget *parent)
{
    return new MoltorinoAuthPage(parent);
}

QTabWidget *moltorinoAuthLoginPageTabs(QWidget *page)
{
    auto *authPage = dynamic_cast<MoltorinoAuthPage *>(page);
    if (authPage == nullptr)
    {
        return nullptr;
    }
    return authPage->tabs();
}

}  // namespace chatterino

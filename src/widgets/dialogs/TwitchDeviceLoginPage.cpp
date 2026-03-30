// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/TwitchDeviceLoginPage.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/twitch/TwitchAccountManager.hpp"
#include "singletons/Settings.hpp"
#include "util/Clipboard.hpp"

#include <pajlada/settings/setting.hpp>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace {

using namespace Qt::Literals::StringLiterals;

constexpr char TWITCH_TV_CLIENT_ID[] = "ue6666qo983tsx6so1t0vnawi233wa";

// Try to mimic the miner's device login headers enough to avoid Twitch quirks.
constexpr char TWITCH_TV_USER_AGENT[] =
    "Mozilla/5.0 (Linux; Android 7.1; Smart Box C1) AppleWebKit/537.36 (KHTML, "
    "like Gecko) Chrome/108.0.0.0 Safari/537.36";

QByteArray formPayload(const QUrlQuery &q)
{
    return q.toString(QUrl::FullyEncoded).toUtf8();
}

QString oauthError(const QJsonObject &json)
{
    const auto err = json.value("error"_L1).toString();
    const auto desc = json.value("error_description"_L1).toString();
    if (!err.isEmpty() && !desc.isEmpty())
    {
        return err + u": "_s + desc;
    }
    if (!desc.isEmpty())
    {
        return desc;
    }
    return err;
}

}  // namespace

namespace chatterino {

TwitchDeviceLoginPage::TwitchDeviceLoginPage(QWidget *parent)
    : BaseWidget(parent)
{
    auto *root = new QVBoxLayout(this);

    this->purposeLabel_ = new QLabel(
        u"This sign in is only needed for channel points features (like "
        u"predictions) to work. If you don't care or plan to use those, you can just use the basic login."_s,
        this);
    this->purposeLabel_->setWordWrap(true);
    root->addWidget(this->purposeLabel_);

    this->instructionsLabel_ = new QLabel(
        u"You'll finish signing in on Twitch's website in your own browser.\n\n"
        u"1) Click Start\n"
        u"2) Open twitch.tv/activate\n"
        u"3) Enter the code shown below and activate\n"
        u"4) Authorize the app"_s,
        this);
    this->instructionsLabel_->setWordWrap(true);
    root->addWidget(this->instructionsLabel_);

    auto *codeRow = new QHBoxLayout;
    this->codeLabel_ = new QLabel(u"<b>Code:</b> —"_s, this);
    this->codeLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    codeRow->addWidget(this->codeLabel_, 1);

    this->copyCodeButton_ = new QPushButton(u"Copy code"_s, this);
    this->copyCodeButton_->setEnabled(false);
    QObject::connect(
        this->copyCodeButton_, &QPushButton::clicked, this, [this] {
            if (!this->hasPending_ || this->pending_.userCode.isEmpty())
            {
                return;
            }
            qApp->clipboard()->setText(this->pending_.userCode);
        });
    codeRow->addWidget(this->copyCodeButton_);

    this->openActivateButton_ =
        new QPushButton(u"Open activation page"_s, this);
    this->openActivateButton_->setEnabled(false);
    QObject::connect(
        this->openActivateButton_, &QPushButton::clicked, this, [this] {
            const QUrl url = this->hasPending_
                                 ? this->pending_.verificationUri
                                 : QUrl(u"https://www.twitch.tv/activate"_s);
            QDesktopServices::openUrl(url);
        });
    codeRow->addWidget(this->openActivateButton_);

    root->addLayout(codeRow);

    this->statusLabel_ = new QLabel(u"Ready."_s, this);
    this->statusLabel_->setWordWrap(true);
    root->addWidget(this->statusLabel_);

    auto *buttons = new QHBoxLayout;
    this->startButton_ = new QPushButton(u"Start"_s, this);
    QObject::connect(this->startButton_, &QPushButton::clicked, this, [this] {
        this->start();
    });
    buttons->addWidget(this->startButton_);

    this->cancelButton_ = new QPushButton(u"Cancel"_s, this);
    this->cancelButton_->setEnabled(false);
    QObject::connect(this->cancelButton_, &QPushButton::clicked, this, [this] {
        this->cancel();
    });
    buttons->addWidget(this->cancelButton_);

    root->addLayout(buttons);

    this->pollTimer_ = new QTimer(this);
    this->pollTimer_->setSingleShot(true);
    QObject::connect(this->pollTimer_, &QTimer::timeout, this, [this] {
        this->pollToken();
    });
}

void TwitchDeviceLoginPage::start()
{
    this->cancelRequested_ = false;
    this->hasPending_ = false;
    this->accessToken_.clear();
    this->codeLabel_->setText(u"<b>Code:</b> —"_s);
    this->setControlsEnabled(false);
    this->copyCodeButton_->setEnabled(false);
    this->openActivateButton_->setEnabled(false);
    this->cancelButton_->setEnabled(true);
    this->setStatus(u"Requesting device code…"_s);
    this->requestDeviceCode();
}

void TwitchDeviceLoginPage::cancel()
{
    this->cancelRequested_ = true;
    this->pollTimer_->stop();
    this->setControlsEnabled(true);
    this->cancelButton_->setEnabled(false);
    this->setStatus(u"Cancelled."_s);
}

void TwitchDeviceLoginPage::setStatus(const QString &text)
{
    if (this->statusLabel_)
    {
        this->statusLabel_->setText(text);
    }
}

void TwitchDeviceLoginPage::setControlsEnabled(bool enabled)
{
    if (this->startButton_)
    {
        this->startButton_->setEnabled(enabled);
    }
}

QString TwitchDeviceLoginPage::buildScopesString() const
{
    QStringList scopes;
    scopes.reserve(static_cast<int>(AUTH_SCOPES.size()));
    for (const auto &s : AUTH_SCOPES)
    {
        scopes.append(QString(s));
    }
    return scopes.join(u' ');
}

void TwitchDeviceLoginPage::requestDeviceCode()
{
    QUrlQuery payload{
        {u"client_id"_s, u""_s + TWITCH_TV_CLIENT_ID},
        {u"scopes"_s, this->buildScopesString()},
    };

    NetworkRequest(QUrl(u"https://id.twitch.tv/oauth2/device"_s),
                   NetworkRequestType::Post)
        .timeout(10'000)
        .header("Accept", "application/json")
        .header("Content-Type", "application/x-www-form-urlencoded")
        .header("Client-Id", TWITCH_TV_CLIENT_ID)
        .header("Origin", "https://android.tv.twitch.tv")
        .header("Referer", "https://android.tv.twitch.tv/")
        .header("User-Agent", TWITCH_TV_USER_AGENT)
        .payload(formPayload(payload))
        .caller(this)
        .onError([this](const NetworkResult &res) {
            this->setControlsEnabled(true);
            this->cancelButton_->setEnabled(false);
            this->setStatus(u"Failed to request device code: "_s +
                            res.formatError());
        })
        .onSuccess([this](const NetworkResult &res) {
            if (this->cancelRequested_)
            {
                return;
            }
            const auto json = res.parseJson();
            const auto deviceCode = json.value("device_code"_L1).toString();
            const auto userCode = json.value("user_code"_L1).toString();
            const auto verification =
                json.value("verification_uri"_L1).toString();
            const auto interval = json.value("interval"_L1).toInt(5);
            const auto expiresIn = json.value("expires_in"_L1).toInt(0);

            if (deviceCode.isEmpty() || userCode.isEmpty() ||
                verification.isEmpty() || expiresIn <= 0)
            {
                this->setControlsEnabled(true);
                this->cancelButton_->setEnabled(false);
                this->setStatus(
                    u"Device code response missing required fields."_s);
                qCWarning(chatterinoTwitch) << "Device code response:" << json;
                return;
            }

            this->pending_.deviceCode = deviceCode;
            this->pending_.userCode = userCode;
            this->pending_.verificationUri = QUrl(verification);
            this->pending_.intervalSeconds = std::max(1, interval);
            this->pending_.expiresAtUtc =
                QDateTime::currentDateTimeUtc().addSecs(expiresIn);
            this->hasPending_ = true;

            this->codeLabel_->setText(u"<b>Code:</b> "_s + userCode);
            this->copyCodeButton_->setEnabled(true);
            this->openActivateButton_->setEnabled(true);

            this->setStatus(u"Waiting for activation…"_s);

            // Sleep first like the miner does.
            this->pollTimer_->start(this->pending_.intervalSeconds * 1000);
        })
        .execute();
}

void TwitchDeviceLoginPage::pollToken()
{
    if (this->cancelRequested_ || !this->hasPending_)
    {
        return;
    }

    if (QDateTime::currentDateTimeUtc() >= this->pending_.expiresAtUtc)
    {
        this->setControlsEnabled(true);
        this->cancelButton_->setEnabled(false);
        this->setStatus(u"Code expired. Please start again."_s);
        return;
    }

    QUrlQuery payload{
        {u"client_id"_s, u""_s + TWITCH_TV_CLIENT_ID},
        {u"device_code"_s, this->pending_.deviceCode},
        {u"grant_type"_s, u"urn:ietf:params:oauth:grant-type:device_code"_s},
    };

    NetworkRequest(QUrl(u"https://id.twitch.tv/oauth2/token"_s),
                   NetworkRequestType::Post)
        .timeout(10'000)
        .header("Accept", "application/json")
        .header("Content-Type", "application/x-www-form-urlencoded")
        .header("Client-Id", TWITCH_TV_CLIENT_ID)
        .header("Origin", "https://android.tv.twitch.tv")
        .header("Referer", "https://android.tv.twitch.tv/")
        .header("User-Agent", TWITCH_TV_USER_AGENT)
        .payload(formPayload(payload))
        .caller(this)
        .onError([this](const NetworkResult &res) {
            if (this->cancelRequested_)
            {
                return;
            }
            // Keep polling on transient errors.
            this->setStatus(u"Polling failed: "_s + res.formatError());
            this->pollTimer_->start(this->pending_.intervalSeconds * 1000);
        })
        .onSuccess([this](const NetworkResult &res) {
            if (this->cancelRequested_)
            {
                return;
            }

            const auto json = res.parseJson();
            const auto token = json.value("access_token"_L1).toString();
            if (!token.isEmpty())
            {
                this->accessToken_ = token;
                this->setStatus(u"Got token. Validating…"_s);
                this->requestValidate(token);
                return;
            }

            const auto err = oauthError(json).toLower();
            if (err.contains(u"authorization_pending"_s))
            {
                this->setStatus(u"Waiting for activation…"_s);
                this->pollTimer_->start(this->pending_.intervalSeconds * 1000);
                return;
            }
            if (err.contains(u"slow_down"_s))
            {
                this->pending_.intervalSeconds =
                    std::min(60, this->pending_.intervalSeconds + 5);
                this->setStatus(u"Please wait…"_s);
                this->pollTimer_->start(this->pending_.intervalSeconds * 1000);
                return;
            }
            if (err.contains(u"expired"_s))
            {
                this->setControlsEnabled(true);
                this->cancelButton_->setEnabled(false);
                this->setStatus(u"Code expired. Please start again."_s);
                return;
            }
            if (err.contains(u"access_denied"_s))
            {
                this->setControlsEnabled(true);
                this->cancelButton_->setEnabled(false);
                this->setStatus(u"Access denied."_s);
                return;
            }

            // Unknown response, stop with details.
            this->setControlsEnabled(true);
            this->cancelButton_->setEnabled(false);
            this->setStatus(u"Unexpected token response."_s);
            qCWarning(chatterinoTwitch) << "Token response:" << json;
        })
        .execute();
}

void TwitchDeviceLoginPage::requestValidate(const QString &accessToken)
{
    NetworkRequest(QUrl(u"https://id.twitch.tv/oauth2/validate"_s),
                   NetworkRequestType::Get)
        .timeout(10'000)
        .header("Accept", "application/json")
        .header("Authorization", u"OAuth "_s + accessToken)
        .caller(this)
        .onError([this](const NetworkResult &res) {
            this->setControlsEnabled(true);
            this->cancelButton_->setEnabled(false);
            this->setStatus(u"Validate failed: "_s + res.formatError());
        })
        .onSuccess([this, accessToken](const NetworkResult &res) {
            const auto json = res.parseJson();
            const auto userId = json.value("user_id"_L1).toString();
            const auto login = json.value("login"_L1).toString();
            if (userId.isEmpty() || login.isEmpty())
            {
                this->setControlsEnabled(true);
                this->cancelButton_->setEnabled(false);
                this->setStatus(u"Validate response missing user info."_s);
                qCWarning(chatterinoTwitch) << "Validate response:" << json;
                return;
            }

            this->persistAccount(userId, login, accessToken);
        })
        .execute();
}

void TwitchDeviceLoginPage::persistAccount(const QString &userId,
                                           const QString &login,
                                           const QString &accessToken)
{
    std::string basePath = "/accounts/uid" + userId.toStdString();
    pajlada::Settings::Setting<QString>::set(basePath + "/username", login);
    pajlada::Settings::Setting<QString>::set(basePath + "/userID", userId);
    pajlada::Settings::Setting<QString>::set(basePath + "/clientID",
                                             u""_s + TWITCH_TV_CLIENT_ID);
    pajlada::Settings::Setting<QString>::set(basePath + "/oauthToken",
                                             accessToken);

    getApp()->getAccounts()->twitch.reloadUsers();
    getApp()->getAccounts()->twitch.currentUsername = login;
    getSettings()->requestSave();

    this->setStatus(u"Logged in as "_s + login + u"."_s);
    this->cancelButton_->setEnabled(false);
    this->setControlsEnabled(true);
    this->window()->close();
}

}  // namespace chatterino

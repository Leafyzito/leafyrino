// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWidget.hpp"

#include <QDateTime>
#include <QUrl>

class QLabel;
class QPushButton;
class QTimer;

namespace chatterino {

class TwitchDeviceLoginPage final : public BaseWidget
{
    Q_OBJECT

public:
    explicit TwitchDeviceLoginPage(QWidget *parent = nullptr);

private:
    struct DeviceCodeResponse {
        QString deviceCode;
        QString userCode;
        QUrl verificationUri;
        int intervalSeconds{5};
        QDateTime expiresAtUtc;
    };

    void start();
    void cancel();

    void requestDeviceCode();
    void pollToken();
    void requestValidate(const QString &accessToken);
    void persistAccount(const QString &userId, const QString &login,
                        const QString &accessToken);

    void setStatus(const QString &text);
    void setControlsEnabled(bool enabled);

    QString buildScopesString() const;

    QString accessToken_;
    DeviceCodeResponse pending_;
    bool hasPending_{false};
    bool cancelRequested_{false};

    QTimer *pollTimer_{nullptr};

    QLabel *statusLabel_{nullptr};
    QLabel *purposeLabel_{nullptr};
    QLabel *instructionsLabel_{nullptr};
    QLabel *codeLabel_{nullptr};
    QPushButton *startButton_{nullptr};
    QPushButton *openActivateButton_{nullptr};
    QPushButton *copyCodeButton_{nullptr};
    QPushButton *cancelButton_{nullptr};
};

}  // namespace chatterino

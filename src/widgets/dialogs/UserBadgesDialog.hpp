// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/twitch/api/TwitchGql.hpp"
#include "widgets/DraggablePopup.hpp"

#include <QPointer>
#include <QString>
#include <QVector>

#include <vector>

class QLabel;
class QLineEdit;
class QResizeEvent;
class QShowEvent;
class QScrollArea;
class QVBoxLayout;

namespace chatterino {

class Button;
class SvgButton;
class TwitchChannel;

class UserBadgesDialog : public DraggablePopup
{
public:
    UserBadgesDialog(const QString &userLogin, const QString &channelLogin,
                     const QString &displayName = {},
                     TwitchChannel *channel = nullptr,
                     QWidget *parent = nullptr);

    static void showDialog(const QString &userLogin,
                           const QString &channelLogin,
                           const QString &displayName = {},
                           TwitchChannel *channel = nullptr,
                           QWidget *parent = nullptr);

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void loadBadges(bool force = false);
    void rebuildContent();
    void clearContent();
    void refreshStyle();
    void setStatus(const QString &text, bool error = false);
    void applySizeConstraints();
    void openBadgeInChatVault(const GqlBadge &badge);
    [[nodiscard]] int badgeGridColumns() const;
    [[nodiscard]] QString authTokenOrMessage();

    QString userLogin_;
    QString channelLogin_;
    QString displayName_;
    TwitchChannel *channel_{};

    QVBoxLayout *mainLayout_{};
    QWidget *headerWidget_{};
    QLabel *headerTitleLabel_{};
    QLineEdit *searchInput_{};
    Button *pinButton_{};
    SvgButton *closeButton_{};
    QScrollArea *scrollArea_{};
    QWidget *contentWidget_{};
    QVBoxLayout *contentLayout_{};
    QLabel *statusLabel_{};

    QVector<GqlBadge> badges_;
    bool badgesLoaded_ = false;
    bool badgesLoading_ = false;
    bool initialFetchDone_ = false;
    QString statusText_;
    bool statusIsError_ = false;
    QString searchQuery_;
    int lastBadgeGridColumns_ = -1;

    static std::vector<QPointer<UserBadgesDialog>> activeDialogs_;
};

}  // namespace chatterino

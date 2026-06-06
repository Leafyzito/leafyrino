// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/moltorino/MoltorinoFeatureFlags.hpp"

#    include "providers/twitch/api/TwitchGql.hpp"
#    include "widgets/DraggablePopup.hpp"

#    include <QPointer>
#    include <QString>
#    include <QTimer>
#    include <QVector>

class QLabel;
class QPushButton;
class QScrollArea;
class QResizeEvent;
class QShowEvent;
class QVBoxLayout;

namespace chatterino {

class SvgButton;
class Button;
class TwitchChannel;

class TwitchBadgePickerDialog : public DraggablePopup
{
public:
    TwitchBadgePickerDialog(TwitchChannel *channel,
                            QWidget *parent = nullptr);

    static void showDialog(TwitchChannel *channel,
                           QWidget *parent = nullptr);

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    enum class View {
        GlobalBadges,
        ChannelBadges,
    };

    void loadBadges(bool force = false);
    void rebuildContent();
    void rebuildGlobalBadges();
    void rebuildChannelBadges();
    void clearContent();
    void deselectChannel();
    void setFlairHidden(bool hidden);
    void refreshStyle();
    void setStatus(const QString &text, bool error = false);
    void selectGlobal(const GqlBadge &badge);
    void selectChannel(const GqlBadge &badge);
    QString authTokenOrMessage();
    void applySizeConstraints();

    TwitchChannel *channel_{};

    QVBoxLayout *mainLayout_{};
    QWidget *headerWidget_{};
    QLabel *headerTitleLabel_{};
    QPushButton *globalTabButton_{};
    QPushButton *channelTabButton_{};
    Button *pinButton_{};
    SvgButton *closeButton_{};
    QScrollArea *scrollArea_{};
    QWidget *contentWidget_{};
    QVBoxLayout *contentLayout_{};
    QLabel *statusLabel_{};

    View view_ = View::GlobalBadges;
    GqlChatSettingsBadges badges_;
    bool badgesLoaded_ = false;
    bool badgesLoading_ = false;
    bool actionInFlight_ = false;
    bool initialFetchDone_ = false;
    QString statusText_;
    bool statusIsError_ = false;
    bool useCustomChannelBadge_ = false;
    bool isBadgeModifierHidden_ = false;

    static std::vector<QPointer<TwitchBadgePickerDialog>> activeDialogs_;
};

}  // namespace chatterino
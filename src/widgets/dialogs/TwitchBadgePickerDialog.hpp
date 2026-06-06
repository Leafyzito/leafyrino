// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/moltorino/MoltorinoFeatureFlags.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "widgets/DraggablePopup.hpp"

#include <QDateTime>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVector>

#include <optional>

class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QResizeEvent;
class QShowEvent;
class QVBoxLayout;

namespace chatterino {

class SvgButton;
class Button;
class TwitchChannel;

struct EventBadge {
    QString id;
    QString name;
    QString imageUrl;
    QString streamDatabaseUrl;
    QDateTime startAt;
    QDateTime endAt;
    std::optional<bool> free;
};

class TwitchBadgePickerDialog : public DraggablePopup
{
public:
    TwitchBadgePickerDialog(TwitchChannel *channel, QWidget *parent = nullptr);

    static void showDialog(TwitchChannel *channel, QWidget *parent = nullptr);

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    enum class View {
        GlobalBadges,
        ChannelBadges,
        EventBadges,
    };

    void loadBadges(bool force = false);
    void loadEventBadges(bool force = false);
    void rebuildContent();
    void rebuildGlobalBadges();
    void rebuildChannelBadges();
    void rebuildEventBadges();
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
    QLineEdit *searchInput_{};
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
    QString searchQuery_;

    QPushButton *eventTabButton_{};
    QVector<EventBadge> eventBadgesCache_;
    bool eventBadgesLoading_ = false;
    bool eventBadgesLoaded_ = false;
    QDateTime eventBadgesCacheTime_;

    static std::vector<QPointer<TwitchBadgePickerDialog>> activeDialogs_;
};

}  // namespace chatterino

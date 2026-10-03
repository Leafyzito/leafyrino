// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BasePopup.hpp"

#include <QJsonArray>
#include <QString>
#include <QTimer>

#include <vector>

class QBuffer;
class QLabel;
class QLineEdit;
class QMovie;
class QPushButton;
class QScrollArea;
class QTabBar;

namespace chatterino {

class GifGrid;
class Label;
class TwitchChannel;

/// Searches Giphy and sends a GIF to a Twitch channel with Twitch's own GIF
/// messages. Twitch decides per channel whether GIFs can be sent and hands
/// out the key for the search.
class GifPickerDialog : public BasePopup
{
public:
    /// Opens the picker for `channel`, or raises the one that is open for it.
    /// A `searchTerm` is searched for right away.
    static void showDialog(TwitchChannel *channel, const QString &searchTerm,
                           QWidget *parent);

private:
    struct Gif {
        QString id;
        /// What Twitch is told to show.
        QString url;
    };

    GifPickerDialog(TwitchChannel *channel, QWidget *parent);

    /// Asks Twitch whether GIFs can be sent here, and for the search key.
    void loadConfig();
    /// Starts over with the search field's text, or the favorites.
    void reload();
    /// Loads the next results after the ones shown.
    void loadPage();
    void addGifs(const QJsonArray &gifs);
    void loadThumbnail(const QString &gifId, int index);
    /// Plays the selected GIF in its place in the grid.
    void selectionChanged();
    void toggleFavorite();
    void sendGif();
    /// Tells why Twitch may have rejected a GIF with `error`.
    void explainSendError(const QString &error);
    void setStatus(const QString &text);
    void updateButtons();
    /// Whether the favorites are shown instead of search results.
    bool showingFavorites() const;

    QString channelName_;
    QString channelId_;
    QString token_;
    QString apiKey_;
    QString rating_;

    std::vector<Gif> gifs_;
    /// How many results there are for the current search.
    int total_ = 0;
    bool loading_ = false;
    bool sending_ = false;
    /// Bumped with every reload, so answers to older ones are dropped.
    int generation_ = 0;
    /// Seconds until Twitch accepts the next GIF.
    int waitSeconds_ = 0;

    QTimer searchTimer_;
    QTimer waitTimer_;

    QTabBar *tabs_{};
    QLineEdit *search_{};
    QScrollArea *scroll_{};
    GifGrid *grid_{};
    Label *status_{};
    /// The guidelines notice, or why the last GIF wasn't sent.
    QLabel *notice_{};
    QPushButton *favorite_{};
    QPushButton *send_{};
    QMovie *movie_{};
    QBuffer *movieData_{};
};

}  // namespace chatterino

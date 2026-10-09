#pragma once

#include "messages/Image.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/api/TwitchGifs.hpp"
#include "widgets/DraggablePopup.hpp"

#include <boost/signals2.hpp>
#include <QHash>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTabBar;

namespace chatterino {

class TwitchChannel;
class GifGridDelegate;

class GifPickerDialog : public DraggablePopup
{
public:
    ~GifPickerDialog() override;
    /// Opens the picker for `channel`, or raises the one already open for it.
    /// A non-empty `searchTerm` fills the search box and starts that search.
    static void showDialog(TwitchChannel *channel, const QString &searchTerm,
                           QWidget *parent = nullptr);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void scaleChangedEvent(float scale) override;
    void themeChangedEvent() override;

private:
    friend class GifGridDelegate;
    explicit GifPickerDialog(TwitchChannel *channel, const QString &searchTerm,
                             QWidget *parent);
    void showSearch(const QString &searchTerm);
    void refreshAccount();
    void resetSearch();
    void loadPage();
    void selectGif();
    void toggleFavorite();
    void sendGif();
    void explainSendRejection(const QString &rejectionCode);
    void showSendError(const QString &message);
    void refreshSend();
    void setStatus(const QString &text);
    void updateViewport();
    void updateHover(int row);
    void releaseImages();
    void updateCaption();

    std::shared_ptr<TwitchChannel> channel_;
    MoltorinoAuthToken auth_;
    twitchgifs::Config config_;
    QVector<twitchgifs::Gif> gifs_;
    QString searchTerm_;
    QString sendError_;
    int generation_ = 0;
    int accountGeneration_ = 0;
    int nextOffset_ = 0;
    bool loading_ = false;
    bool configLoading_ = false;
    QTimer debounce_;
    QTimer cooldownTimer_;
    QTimer viewportUpdate_;
    QHash<int, ImagePtr> thumbnails_;
    ImagePtr hoverImage_;
    int hoveredRow_ = -1;
    QVector<int> rowTops_;
    int layoutWidth_ = -1;
    int layoutCount_ = -1;
    int columns_ = 1;
    QLineEdit *search_{};
    QTabBar *tabs_{};
    QListWidget *grid_{};
    GifGridDelegate *delegate_{};
    QLabel *status_{};
    QLabel *caption_{};
    QPushButton *retry_{};
    QPushButton *favorite_{};
    QPushButton *send_{};
    std::vector<boost::signals2::scoped_connection> boostConnections_;
};

}  // namespace chatterino

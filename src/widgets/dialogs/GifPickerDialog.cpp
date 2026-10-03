// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/GifPickerDialog.hpp"

#include "common/Literals.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "widgets/Label.hpp"

#include <QBuffer>
#include <QCursor>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMovie>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTabBar>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

namespace chatterino {

using namespace literals;

namespace {

constexpr int TIMEOUT_MS = 15000;
// Not PAGE_SIZE: that is a macro on FreeBSD.
constexpr int RESULTS_PER_PAGE = 24;
constexpr int MAX_FAVORITES = 50;
constexpr QSize DIALOG_SIZE(500, 700);
constexpr int FAVORITES_TAB = 1;

const QString GUIDELINES_NOTICE =
    u"GIFs you send must follow Twitch's Community Guidelines."_s;
const QString GIPHY_API = u"https://api.giphy.com/v1/gifs"_s;
const QString GIPHY_MEDIA = u"https://media.giphy.com/media/"_s;

/// The open pickers, so a channel only gets one.
std::vector<std::pair<QString, QPointer<GifPickerDialog>>> &openDialogs()
{
    static std::vector<std::pair<QString, QPointer<GifPickerDialog>>> dialogs;
    return dialogs;
}

QStringList favoriteIds()
{
    return getSettings()->twitchGifFavorites.getValue().split(
        u',', Qt::SkipEmptyParts);
}

/// Giphy's name for the content rating Twitch allows in the channel.
QString giphyRating(const QString &twitchRating)
{
    return twitchRating == u"PG_13" ? u"pg-13"_s : u"pg"_s;
}

}  // namespace

/// The GIFs, in rows that fill the width: every row is as high as it has to
/// be for its GIFs to reach from edge to edge.
class GifGrid : public QWidget
{
public:
    using QWidget::QWidget;

    std::function<void()> selectionChanged;
    std::function<void()> activated;

    void clear()
    {
        this->items_.clear();
        this->selected_ = -1;
        this->frame_ = {};
        this->relayout();
    }

    /// Returns the index of the new GIF.
    int add(const QString &id, QSize size)
    {
        this->items_.push_back({
            .id = id,
            .size = size.isEmpty() ? QSize(4, 3) : size,
        });
        return static_cast<int>(this->items_.size()) - 1;
    }

    void setStill(int index, const QString &id, const QPixmap &pixmap)
    {
        if (index >= 0 && std::cmp_less(index, this->items_.size()) &&
            this->items_[index].id == id)
        {
            this->items_[index].still = pixmap;
            this->update(this->items_[index].rect);
        }
    }

    /// The current frame of the selected GIF, drawn in place of its still.
    void setFrame(const QPixmap &frame)
    {
        this->frame_ = frame;
        if (this->selected_ >= 0)
        {
            this->update(this->items_[this->selected_].rect);
        }
    }

    QString selectedId() const
    {
        return this->selected_ < 0 ? QString()
                                   : this->items_[this->selected_].id;
    }

    void relayout()
    {
        constexpr int spacing = 3;
        const int width = std::max(1, this->width());
        const double targetHeight = 150;

        int y = 0;
        size_t rowStart = 0;
        double rowWidth = 0;
        const auto place = [&](size_t end, double height) {
            double x = 0;
            for (auto i = rowStart; i < end; ++i)
            {
                auto &item = this->items_[i];
                const auto w = height * item.size.width() / item.size.height();
                item.rect = QRectF(x, y, w, height).toRect();
                x += w + spacing;
            }
            y += qRound(height) + spacing;
            rowStart = end;
            rowWidth = 0;
        };
        for (size_t i = 0; i < this->items_.size(); ++i)
        {
            const auto &size = this->items_[i].size;
            rowWidth += targetHeight * size.width() / size.height();
            const auto gaps = static_cast<double>(i - rowStart) * spacing;
            if (rowWidth + gaps >= width)
            {
                // Full: shrink the row so it ends at the right edge.
                place(i + 1, targetHeight * (width - gaps) / rowWidth);
            }
        }
        // The last row may not be full; it keeps the usual height.
        place(this->items_.size(), targetHeight);

        this->setMinimumHeight(y);
        this->update();
    }

protected:
    void resizeEvent(QResizeEvent * /*event*/) override
    {
        this->relayout();
    }

    void paintEvent(QPaintEvent *event) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        for (int i = 0; std::cmp_less(i, this->items_.size()); ++i)
        {
            const auto &item = this->items_[i];
            if (!item.rect.intersects(event->rect()))
            {
                continue;
            }
            const bool selected = i == this->selected_;
            const auto &pixmap =
                selected && !this->frame_.isNull() ? this->frame_ : item.still;
            if (pixmap.isNull())
            {
                painter.fillRect(item.rect, QColor(128, 128, 128, 40));
            }
            else
            {
                painter.drawPixmap(item.rect, pixmap);
            }
            if (selected)
            {
                painter.setPen(QPen(this->palette().highlight(), 3));
                painter.drawRect(item.rect.adjusted(1, 1, -2, -2));
            }
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton)
        {
            return;
        }
        const auto pos = event->position().toPoint();
        const auto it = std::ranges::find_if(this->items_, [&](const auto &i) {
            return i.rect.contains(pos);
        });
        const int index =
            it == this->items_.end()
                ? -1
                : static_cast<int>(std::distance(this->items_.begin(), it));
        if (index == this->selected_)
        {
            return;
        }
        this->selected_ = index;
        this->frame_ = {};
        this->update();
        if (this->selectionChanged)
        {
            this->selectionChanged();
        }
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && this->selected_ >= 0 &&
            this->activated)
        {
            this->activated();
        }
    }

private:
    struct Item {
        QString id;
        /// Only the proportions matter.
        QSize size;
        QPixmap still;
        QRect rect;
    };

    std::vector<Item> items_;
    int selected_ = -1;
    QPixmap frame_;
};

void GifPickerDialog::showDialog(TwitchChannel *channel,
                                 const QString &searchTerm, QWidget *parent)
{
    if (channel == nullptr)
    {
        return;
    }

    auto &dialogs = openDialogs();
    std::erase_if(dialogs, [](const auto &entry) {
        return entry.second.isNull();
    });
    for (const auto &[name, dialog] : dialogs)
    {
        if (name == channel->getName())
        {
            if (!searchTerm.isEmpty())
            {
                dialog->search_->setText(searchTerm);
            }
            dialog->raise();
            dialog->activateWindow();
            return;
        }
    }

    auto *dialog = new GifPickerDialog(channel, parent);
    dialogs.emplace_back(channel->getName(), dialog);
    dialog->search_->setText(searchTerm);
    dialog->searchTimer_.stop();

    QPoint center = QCursor::pos();
    if (parent != nullptr && parent->window() != nullptr)
    {
        center = parent->window()->geometry().center();
    }
    dialog->show();
    const auto size = dialog->size();
    dialog->showAndMoveTo(center - QPoint(size.width() / 2, size.height() / 2),
                          widgets::BoundsChecking::DesiredPosition);
    dialog->raise();
    dialog->activateWindow();
    dialog->search_->setFocus();
    dialog->loadConfig();
}

GifPickerDialog::GifPickerDialog(TwitchChannel *channel, QWidget *parent)
    : BasePopup({BaseWindow::EnableCustomFrame, BaseWindow::DisableLayoutSave},
                parent)
    , channelName_(channel->getName())
    , channelId_(channel->roomId())
    , tabs_(new QTabBar(this))
    , search_(new QLineEdit(this))
    , scroll_(new QScrollArea(this))
    , grid_(new GifGrid(this))
    , status_(new Label(this))
    , notice_(new QLabel(GUIDELINES_NOTICE, this))
    , favorite_(new QPushButton("Favorite", this))
    , send_(new QPushButton("Send", this))
    , movie_(new QMovie(this))
    , movieData_(new QBuffer(this))
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowTitle(u"GIFs in #%1"_s.arg(this->channelName_));
    this->setScaleIndependentSize(DIALOG_SIZE);

    auto *layout = new QVBoxLayout(this->getLayoutContainer());

    this->tabs_->addTab("Search");
    this->tabs_->addTab("Favorites");
    this->tabs_->setExpanding(true);
    this->tabs_->setDocumentMode(true);
    this->tabs_->setDrawBase(false);
    layout->addWidget(this->tabs_);

    this->search_->setPlaceholderText("Search Giphy");
    this->search_->setClearButtonEnabled(true);
    layout->addWidget(this->search_);

    this->scroll_->setWidget(this->grid_);
    this->scroll_->setWidgetResizable(true);
    this->scroll_->setFrameShape(QFrame::NoFrame);
    this->scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(this->scroll_, 1);

    this->status_->setCentered(true);
    this->status_->setSizePolicy(QSizePolicy::Expanding,
                                 QSizePolicy::Expanding);
    this->status_->hide();
    layout->addWidget(this->status_, 1);

    auto *buttons = new QHBoxLayout();
    this->notice_->setWordWrap(true);
    buttons->addWidget(this->notice_, 1);
    buttons->addWidget(this->favorite_);
    buttons->addWidget(this->send_);
    layout->addLayout(buttons);

    // Search a moment after the last keystroke.
    this->searchTimer_.setSingleShot(true);
    this->searchTimer_.setInterval(std::chrono::milliseconds(400));
    QObject::connect(&this->searchTimer_, &QTimer::timeout, this, [this] {
        this->reload();
    });
    this->waitTimer_.setInterval(std::chrono::seconds(1));
    QObject::connect(&this->waitTimer_, &QTimer::timeout, this, [this] {
        if (--this->waitSeconds_ <= 0)
        {
            this->waitSeconds_ = 0;
            this->waitTimer_.stop();
        }
        this->updateButtons();
    });

    QObject::connect(this->search_, &QLineEdit::textChanged, this, [this] {
        const QSignalBlocker blocker(this->tabs_);
        this->tabs_->setCurrentIndex(0);
        this->searchTimer_.start();
    });
    QObject::connect(this->search_, &QLineEdit::returnPressed, this, [this] {
        this->searchTimer_.stop();
        this->reload();
    });
    QObject::connect(this->tabs_, &QTabBar::currentChanged, this, [this] {
        this->searchTimer_.stop();
        this->reload();
    });
    this->grid_->selectionChanged = [this] {
        this->selectionChanged();
    };
    this->grid_->activated = [this] {
        this->sendGif();
    };
    // The selected GIF plays in its place in the grid.
    QObject::connect(this->movie_, &QMovie::frameChanged, this, [this] {
        this->grid_->setFrame(this->movie_->currentPixmap());
    });
    // More results when the end of the list comes close.
    QObject::connect(this->scroll_->verticalScrollBar(),
                     &QScrollBar::valueChanged, this, [this](int value) {
                         const auto *bar = this->scroll_->verticalScrollBar();
                         if (value >= bar->maximum() - 200)
                         {
                             this->loadPage();
                         }
                     });
    QObject::connect(this->favorite_, &QPushButton::clicked, this, [this] {
        this->toggleFavorite();
    });
    QObject::connect(this->send_, &QPushButton::clicked, this, [this] {
        this->sendGif();
    });

    this->updateButtons();
}

bool GifPickerDialog::showingFavorites() const
{
    return this->tabs_->currentIndex() == FAVORITES_TAB;
}

void GifPickerDialog::loadConfig()
{
    if (this->channelId_.isEmpty())
    {
        this->setStatus(
            "This channel is still connecting. Try again in a moment.");
        return;
    }
    QString error;
    const auto auth = MoltorinoAuth::resolveCurrentUserToken(&error);
    if (!auth.hasToken())
    {
        this->setStatus(error.isEmpty()
                            ? MoltorinoAuth::authRequiredMessage("sending GIFs")
                            : error);
        return;
    }
    this->token_ = auth.token;
    this->setStatus("Loading GIFs...");

    const QPointer<GifPickerDialog> self(this);
    TwitchGql::getGifPickerConfig(
        this->channelId_, this->token_,
        [self](const GqlGifPickerConfig &config) {
            if (!self)
            {
                return;
            }
            if (!config.enabled)
            {
                self->setStatus("GIFs are not available in this channel.");
                return;
            }
            if (config.apiKey.isEmpty())
            {
                self->setStatus("Twitch did not provide a GIF search key. Try "
                                "again later.");
                return;
            }
            self->apiKey_ = config.apiKey;
            self->rating_ = giphyRating(config.contentRating);
            self->reload();
        },
        [self](const QString &message) {
            if (self)
            {
                self->setStatus(
                    MoltorinoAuth::normalizeAuthError("loading GIFs", message));
            }
        });
}

void GifPickerDialog::reload()
{
    if (this->apiKey_.isEmpty())
    {
        return;
    }
    ++this->generation_;
    this->loading_ = false;
    this->gifs_.clear();
    this->total_ = 0;
    this->grid_->clear();
    this->selectionChanged();
    this->loadPage();
}

void GifPickerDialog::loadPage()
{
    if (this->loading_ || this->apiKey_.isEmpty())
    {
        return;
    }
    const auto offset = static_cast<int>(this->gifs_.size());
    if (offset > 0 && offset >= this->total_)
    {
        return;
    }

    const auto term = this->search_->text().trimmed();
    QUrl url;
    QUrlQuery query;
    query.addQueryItem(u"api_key"_s, this->apiKey_);
    query.addQueryItem(u"rating"_s, this->rating_);
    if (this->showingFavorites())
    {
        const auto ids = favoriteIds();
        if (ids.isEmpty())
        {
            this->setStatus("No favorites yet. Select a GIF and click "
                            "Favorite to save it here.");
            return;
        }
        if (offset > 0)
        {
            // All favorites come in one request.
            return;
        }
        url = QUrl(GIPHY_API);
        query.addQueryItem(u"ids"_s, ids.join(u','));
    }
    else
    {
        url =
            QUrl(GIPHY_API + (term.isEmpty() ? u"/trending"_s : u"/search"_s));
        if (!term.isEmpty())
        {
            query.addQueryItem(
                u"q"_s, QString::fromUtf8(QUrl::toPercentEncoding(term)));
        }
        query.addQueryItem(u"limit"_s, QString::number(RESULTS_PER_PAGE));
        query.addQueryItem(u"offset"_s, QString::number(offset));
    }
    url.setQuery(query);

    this->loading_ = true;
    if (offset == 0)
    {
        this->setStatus("Loading GIFs...");
    }

    const QPointer<GifPickerDialog> self(this);
    const auto generation = this->generation_;
    NetworkRequest(url)
        .timeout(TIMEOUT_MS)
        .onSuccess([self, generation](const NetworkResult &result) {
            if (!self || self->generation_ != generation)
            {
                return;
            }
            self->loading_ = false;
            const auto root = result.parseJson();
            const auto gifs = root.value("data").toArray();
            self->total_ = root.value("pagination")
                               .toObject()
                               .value("total_count")
                               .toInt(static_cast<int>(gifs.size()));
            self->addGifs(gifs);
        })
        .onError([self, generation](const NetworkResult & /*result*/) {
            if (!self || self->generation_ != generation)
            {
                return;
            }
            self->loading_ = false;
            if (self->gifs_.empty())
            {
                self->setStatus(
                    "Could not reach Giphy. Try again in a moment.");
            }
        })
        .execute();
}

void GifPickerDialog::addGifs(const QJsonArray &gifs)
{
    for (const auto &value : gifs)
    {
        const auto gif = value.toObject();
        const auto id = gif.value("id").toString();
        if (id.isEmpty())
        {
            continue;
        }
        const auto images = gif.value("images").toObject();
        // Twitch wants the address the search gave us: it carries the key
        // the GIF was found with.
        auto url = images.value("original").toObject().value("url").toString();
        if (url.isEmpty())
        {
            url = GIPHY_MEDIA + id + u"/giphy.gif"_s;
        }
        this->gifs_.push_back({
            .id = id,
            .url = url,
        });

        // Giphy gives the sizes as strings.
        const auto image = images.value("fixed_width").toObject();
        const QSize size(image.value("width").toString().toInt(),
                         image.value("height").toString().toInt());
        this->loadThumbnail(id, this->grid_->add(id, size));
    }
    this->grid_->relayout();

    if (!this->gifs_.empty())
    {
        this->setStatus({});
    }
    else if (this->showingFavorites())
    {
        this->setStatus("Your favorite GIFs are not available anymore.");
    }
    else
    {
        this->setStatus("No GIFs found. Try another search.");
    }
}

void GifPickerDialog::loadThumbnail(const QString &gifId, int index)
{
    const QPointer<GifPickerDialog> self(this);
    const auto generation = this->generation_;
    // A still image; only the selected GIF plays.
    NetworkRequest(QUrl(GIPHY_MEDIA + gifId + u"/200w_s.gif"_s))
        .timeout(TIMEOUT_MS)
        .cache()
        .onSuccess(
            [self, generation, gifId, index](const NetworkResult &result) {
                QPixmap pixmap;
                if (self && self->generation_ == generation &&
                    pixmap.loadFromData(result.getData()))
                {
                    self->grid_->setStill(index, gifId, pixmap);
                }
            })
        .execute();
}

void GifPickerDialog::selectionChanged()
{
    this->movie_->stop();
    this->updateButtons();

    const auto gifId = this->grid_->selectedId();
    if (gifId.isEmpty())
    {
        return;
    }

    const QPointer<GifPickerDialog> self(this);
    NetworkRequest(QUrl(GIPHY_MEDIA + gifId + u"/200w.gif"_s))
        .timeout(TIMEOUT_MS)
        .cache()
        .onSuccess([self, gifId](const NetworkResult &result) {
            // Still the selected one?
            if (!self || self->grid_->selectedId() != gifId)
            {
                return;
            }
            self->movie_->stop();
            self->movieData_->close();
            self->movieData_->setData(result.getData());
            self->movieData_->open(QIODevice::ReadOnly);
            self->movie_->setDevice(self->movieData_);
            self->movie_->start();
        })
        .execute();
}

void GifPickerDialog::toggleFavorite()
{
    const auto gifId = this->grid_->selectedId();
    if (gifId.isEmpty())
    {
        return;
    }
    auto ids = favoriteIds();
    if (ids.removeAll(gifId) == 0)
    {
        if (ids.size() >= MAX_FAVORITES)
        {
            this->setStatus("Your favorites are full. Remove one before "
                            "adding another.");
            return;
        }
        ids.prepend(gifId);
    }
    getSettings()->twitchGifFavorites = ids.join(u',');

    if (this->showingFavorites())
    {
        this->reload();
    }
    else
    {
        this->updateButtons();
    }
}

void GifPickerDialog::sendGif()
{
    const auto gifId = this->grid_->selectedId();
    if (gifId.isEmpty() || this->sending_ || this->waitSeconds_ > 0)
    {
        return;
    }
    const auto gif = std::ranges::find(this->gifs_, gifId, &Gif::id);
    if (gif == this->gifs_.end())
    {
        return;
    }

    this->sending_ = true;
    this->notice_->setText(GUIDELINES_NOTICE);
    this->updateButtons();

    const QPointer<GifPickerDialog> self(this);
    TwitchGql::sendGifMessage(
        this->channelId_, gif->id, gif->url, this->search_->text().trimmed(),
        this->token_,
        [self](const GqlSendGifResult &result) {
            if (!self)
            {
                return;
            }
            self->sending_ = false;
            if (result.secondsUntilCanSend > 0)
            {
                self->waitSeconds_ = result.secondsUntilCanSend;
                self->waitTimer_.start();
            }
            if (result.error.isEmpty() && !result.messageId.isEmpty())
            {
                // Sent; the GIF shows up in the chat.
                self->close();
                return;
            }

            QString message;
            if (result.error == u"TEMPORARILY_UNAVAILABLE")
            {
                message =
                    u"GIFs are temporarily unavailable. Try again later."_s;
            }
            else if (!result.error.isEmpty())
            {
                // Twitch doesn't say why. Mostly it's the subscription, so
                // look that up to tell the user more.
                self->explainSendError(result.error);
                self->updateButtons();
                return;
            }
            else if (result.secondsUntilCanSend > 0)
            {
                message = u"Wait before sending another GIF."_s;
            }
            else
            {
                message = u"Could not confirm whether the GIF was sent. "
                          "Check chat before trying again."_s;
            }
            self->notice_->setText(message);
            self->updateButtons();
        },
        [self](const QString &message) {
            if (!self)
            {
                return;
            }
            self->sending_ = false;
            self->notice_->setText(
                MoltorinoAuth::normalizeAuthError("sending the GIF", message));
            self->updateButtons();
        });
}

void GifPickerDialog::explainSendError(const QString &error)
{
    this->notice_->setText(u"Twitch rejected the GIF (%1)."_s.arg(error));

    const QPointer<GifPickerDialog> self(this);
    TwitchGql::getOwnSubscriptionTier(
        this->channelId_, this->token_,
        [self, error](int tier) {
            if (!self)
            {
                return;
            }
            if (tier >= 2)
            {
                self->notice_->setText(
                    u"Twitch rejected the GIF (%1), although you have a Tier "
                    "%2 sub here. GIFs may be limited to a higher tier or "
                    "turned off for this channel."_s.arg(error)
                        .arg(tier));
                return;
            }
            self->notice_->setText(
                u"Tier 2+ sub required. You have %1 in this channel."_s.arg(
                    tier == 1 ? u"a Tier 1 sub"_s : u"no sub"_s));
        },
        [self, error](const QString & /*message*/) {
            if (self)
            {
                self->notice_->setText(
                    u"Twitch rejected the GIF (%1). Most channels need a "
                    "Tier 2+ sub for GIFs; your sub here could not be "
                    "checked."_s.arg(error));
            }
        });
}

void GifPickerDialog::setStatus(const QString &text)
{
    // The message takes the place of the GIFs.
    this->status_->setText(text);
    this->status_->setVisible(!text.isEmpty());
    this->scroll_->setVisible(text.isEmpty());
}

void GifPickerDialog::updateButtons()
{
    const auto gifId = this->grid_->selectedId();
    const bool selected = !gifId.isEmpty();

    this->favorite_->setEnabled(selected);
    this->favorite_->setText(
        selected && favoriteIds().contains(gifId) ? "Unfavorite" : "Favorite");

    QString sendText = u"Send"_s;
    if (this->sending_)
    {
        sendText = u"Sending..."_s;
    }
    else if (this->waitSeconds_ > 0)
    {
        sendText = u"Wait %1s"_s.arg(this->waitSeconds_);
    }
    this->send_->setEnabled(selected && !this->sending_ &&
                            this->waitSeconds_ <= 0);
    this->send_->setText(sendText);
}

}  // namespace chatterino

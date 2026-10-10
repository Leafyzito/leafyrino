#include "widgets/dialogs/GifPickerDialog.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Image.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"

#include <QApplication>
#include <QCursor>
#include <QDeadlineTimer>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QVBoxLayout>

#include <algorithm>

namespace chatterino {
namespace {

struct SendState {
    bool pending = false;
    QDeadlineTimer cooldown{0};
};

QHash<QString, SendState> sends;
QList<QPointer<GifPickerDialog>> dialogs;

class GifListWidget : public QListWidget
{
public:
    using QListWidget::QListWidget;

    void relayoutItems()
    {
        this->doItemsLayout();
    }
};

QString sendKey(const MoltorinoAuthToken &auth, const TwitchChannel &channel)
{
    return auth.userId + ':' + channel.roomId();
}

void pruneSends()
{
    sends.removeIf([](const auto &entry) {
        return !entry.value().pending && entry.value().cooldown.hasExpired();
    });
}

void drawGif(QPainter &painter, const QRect &bounds, const QPixmap &pixmap)
{
    const auto size = pixmap.size().scaled(bounds.size(), Qt::KeepAspectRatio);
    const QRect target(bounds.x() + (bounds.width() - size.width()) / 2,
                       bounds.y() + (bounds.height() - size.height()) / 2,
                       size.width(), size.height());
    painter.drawPixmap(target, pixmap);
}

QString subscriptionRejectionMessage(const QString &error, int tier)
{
    if (tier >= 2)
    {
        return QStringLiteral(
                   "Twitch rejected the GIF (%1), although you have a Tier "
                   "%2 sub here. GIFs may be limited to a higher tier or "
                   "turned off for this channel.")
            .arg(error)
            .arg(tier);
    }
    return QStringLiteral("Tier 2+ sub required. You have %1 in this channel.")
        .arg(tier == 1 ? QStringLiteral("a Tier 1 sub")
                       : QStringLiteral("no sub"));
}

QString subscriptionLookupFailedMessage(const QString &error)
{
    return QStringLiteral(
               "Twitch rejected the GIF (%1). Most channels need a "
               "Tier 2+ sub for GIFs; your sub here could not be checked.")
        .arg(error);
}

Url giphyMedia(const QString &id, const QString &file)
{
    return Url{"https://media.giphy.com/media/" + id + "/" + file};
}

}  // namespace

class GifGridDelegate : public QStyledItemDelegate
{
public:
    explicit GifGridDelegate(GifPickerDialog &dialog)
        : QStyledItemDelegate(dialog.grid_)
        , dialog_(dialog)
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setClipRect(option.rect);
        auto background = option;
        this->initStyleOption(&background, index);
        auto image = this->dialog_.thumbnails_.value(index.row());
        if (index.row() == this->dialog_.hoveredRow_ &&
            this->dialog_.hoverImage_ && this->dialog_.isActiveWindow())
        {
            this->dialog_.hoverImage_->load();
            if (this->dialog_.hoverImage_->loaded())
            {
                image = this->dialog_.hoverImage_;
            }
        }
        const auto pixmap = image ? image->pixmapOrLoad() : std::nullopt;
        if (pixmap)
        {
            background.text.clear();
        }
        QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &background,
                                           painter, option.widget);
        const auto bounds = option.rect.adjusted(1, 1, -1, -1);
        if (pixmap)
        {
            drawGif(*painter, bounds, *pixmap);
        }
        if (option.state & (QStyle::State_Selected | QStyle::State_MouseOver))
        {
            painter->save();
            painter->setPen(QPen(this->dialog_.theme->accent, 2));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(bounds.adjusted(1, 1, -1, -1));
            painter->restore();
        }
        painter->restore();
    }

private:
    GifPickerDialog &dialog_;
};

GifPickerDialog::GifPickerDialog(TwitchChannel *channel,
                                 const QString &searchTerm, QWidget *parent)
    : DraggablePopup(false, parent)
    , channel_(channel->sharedFromThis())
{
    this->setObjectName("GifPickerDialog");
    this->setWindowTitle("GIFs in #" + channel->getName());
    this->getLayoutContainer()->setObjectName("GifPickerBody");
    auto *layout = new QVBoxLayout(this->getLayoutContainer());
    this->tabs_ = new QTabBar(this);
    this->tabs_->addTab("Search");
    this->tabs_->addTab("Favorites");
    layout->addWidget(this->tabs_);
    this->search_ = new QLineEdit(this);
    this->search_->setPlaceholderText("Search Giphy");
    this->search_->setAccessibleName("Search GIFs");
    this->search_->setMaxLength(twitchgifs::MAX_SEARCH_LENGTH);
    this->search_->setClearButtonEnabled(true);
    this->search_->setText(searchTerm);
    layout->addWidget(this->search_);
    auto *grid = new GifListWidget(this);
    this->grid_ = grid;
    this->grid_->setViewMode(QListView::IconMode);
    this->grid_->setFlow(QListView::LeftToRight);
    this->grid_->setWrapping(true);
    this->grid_->setResizeMode(QListView::Adjust);
    this->grid_->setMovement(QListView::Static);
    this->grid_->setSelectionMode(QAbstractItemView::SingleSelection);
    this->grid_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->grid_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    this->grid_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    this->grid_->setUniformItemSizes(false);
    this->grid_->setSpacing(0);
    this->grid_->setMouseTracking(true);
    this->grid_->viewport()->installEventFilter(this);
    this->grid_->setAccessibleName("GIF results");
    this->delegate_ = new GifGridDelegate(*this);
    this->grid_->setItemDelegate(this->delegate_);
    layout->addWidget(this->grid_, 1);
    this->retry_ = new QPushButton("Try again", this);
    this->retry_->hide();
    layout->addWidget(this->retry_);
    this->status_ = new QLabel(this);
    this->status_->setTextFormat(Qt::PlainText);
    this->status_->setWordWrap(true);
    layout->addWidget(this->status_);
    auto *footer = new QHBoxLayout;
    this->caption_ = new QLabel(this);
    this->caption_->setTextFormat(Qt::PlainText);
    this->caption_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    this->caption_->setMinimumWidth(0);
    this->caption_->installEventFilter(this);
    footer->addWidget(this->caption_, 1);
    this->favorite_ = new QPushButton("Favorite", this);
    this->favorite_->setEnabled(false);
    footer->addWidget(this->favorite_);
    this->send_ = new QPushButton("Send", this);
    this->send_->setEnabled(false);
    this->send_->setToolTip(
        "GIFs you send must follow Twitch's Community Guidelines.");
    footer->addWidget(this->send_);
    layout->addLayout(footer);

    this->debounce_.setSingleShot(true);
    this->debounce_.setInterval(300);
    QObject::connect(&this->debounce_, &QTimer::timeout, this,
                     &GifPickerDialog::loadPage);
    QObject::connect(this->search_, &QLineEdit::textChanged, this, [this] {
        this->resetSearch();
        this->debounce_.start();
    });
    QObject::connect(this->tabs_, &QTabBar::currentChanged, this, [this] {
        this->search_->setVisible(this->tabs_->currentIndex() == 0);
        this->resetSearch();
        this->loadPage();
    });
    QObject::connect(this->grid_, &QListWidget::currentRowChanged, this,
                     &GifPickerDialog::selectGif);
    QObject::connect(this->retry_, &QPushButton::clicked, this, [this] {
        this->retry_->hide();
        if (this->config_.apiKey.isEmpty())
        {
            this->refreshAccount();
        }
        else
        {
            this->loadPage();
        }
    });
    QObject::connect(this->favorite_, &QPushButton::clicked, this,
                     &GifPickerDialog::toggleFavorite);
    QObject::connect(this->send_, &QPushButton::clicked, this,
                     &GifPickerDialog::sendGif);
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    QObject::connect(escape, &QShortcut::activated, this, &QWidget::close);
    this->cooldownTimer_.setInterval(1000);
    QObject::connect(&this->cooldownTimer_, &QTimer::timeout, this,
                     &GifPickerDialog::refreshSend);
    this->viewportUpdate_.setSingleShot(true);
    QObject::connect(&this->viewportUpdate_, &QTimer::timeout, this,
                     &GifPickerDialog::updateViewport);
    QObject::connect(this->grid_->verticalScrollBar(),
                     &QScrollBar::valueChanged, this, [this] {
                         this->viewportUpdate_.start(0);
                     });
    this->installEventFilter(this);
    this->boostConnections_.emplace_back(
        getApp()->getAccounts()->twitch.currentUserChanged.connect([this] {
            this->refreshAccount();
        }));
    getSettings()->moltorinoAuthAccounts.connect(
        [this](const QString &, auto) {
            this->refreshAccount();
        },
        this->signalHolder_);
    getSettings()->customPinAuthToken.connect(
        [this](const QString &, auto) {
            this->refreshAccount();
        },
        this->signalHolder_);
    this->signalHolder_.managedConnect(
        getApp()->getWindows()->layoutRequested, [this](auto *channel) {
            if (channel == nullptr && this->isVisible())
            {
                this->grid_->viewport()->update();
            }
        });
    this->signalHolder_.managedConnect(
        getApp()->getWindows()->gifRepaintRequested, [this] {
            if (this->hoverImage_ && this->isVisible() &&
                this->isActiveWindow() && this->hoveredRow_ >= 0 &&
                this->hoveredRow_ < this->grid_->count())
            {
                this->grid_->viewport()->update(this->grid_->visualItemRect(
                    this->grid_->item(this->hoveredRow_)));
            }
        });
    this->scaleChangedEvent(this->scale());
    this->resize(qRound(420 * this->scale()), qRound(570 * this->scale()));
    this->themeChangedEvent();
    this->refreshAccount();
}

void GifPickerDialog::themeChangedEvent()
{
    DraggablePopup::themeChangedEvent();
    auto palette = this->palette();
    palette.setColor(QPalette::Window, this->theme->window.background);
    palette.setColor(QPalette::WindowText, this->theme->window.text);
    palette.setColor(QPalette::Base, this->theme->splits.input.background);
    palette.setColor(QPalette::Text, this->theme->window.text);
    palette.setColor(QPalette::Button, this->theme->splits.input.background);
    palette.setColor(QPalette::ButtonText, this->theme->window.text);
    palette.setColor(QPalette::Highlight,
                     this->theme->splits.header.focusedBorder);
    palette.setColor(QPalette::HighlightedText, this->theme->window.text);
    auto muted = this->theme->window.text;
    muted.setAlpha(150);
    palette.setColor(QPalette::PlaceholderText, muted);
    this->setPalette(palette);
    this->setStyleSheet(
        QStringLiteral(R"(
        QWidget#GifPickerBody QLabel { color: %2; }
        QTabBar::tab {
            background: %1; color: %2;
            border-top: 2px solid transparent; padding: 4px;
        }
        QTabBar::tab:selected { border-top-color: %4; font-weight: bold; }
        QLineEdit, QListWidget, QPushButton {
            background: %1; color: %2;
            border: 1px solid %3; border-radius: 3px;
        }
        QLineEdit { padding: 3px; }
        QPushButton { padding: 3px 8px; }
        QPushButton:disabled { color: %5; }
        QListWidget { border: none; border-radius: 0; }
        QScrollBar:vertical { background: %6; width: %9px; margin: 0; }
        QScrollBar::handle:vertical { background: %7; min-height: 20px; }
        QScrollBar::handle:vertical:hover, QScrollBar::handle:vertical:pressed {
            background: %8;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0; width: 0;
        }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
            background: transparent;
        }
    )")
            .arg(this->theme->splits.input.background.name(),
                 this->theme->window.text.name(),
                 this->theme->splits.header.border.name(),
                 this->theme->accent.name(), muted.name(QColor::HexArgb),
                 this->theme->scrollbars.background.name(QColor::HexArgb),
                 this->theme->scrollbars.thumb.name(QColor::HexArgb),
                 this->theme->scrollbars.thumbSelected.name(QColor::HexArgb))
            .arg(qRound(8 * this->scale())));
}

void GifPickerDialog::showDialog(TwitchChannel *channel,
                                 const QString &searchTerm, QWidget *parent)
{
    dialogs.removeIf([](const auto &dialog) {
        return dialog.isNull();
    });
    for (const auto &dialog : dialogs)
    {
        if (dialog->channel_.get() == channel)
        {
            if (!searchTerm.isEmpty())
            {
                dialog->showSearch(searchTerm);
            }
            dialog->show();
            dialog->raise();
            dialog->activateWindow();
            dialog->search_->setFocus();
            return;
        }
    }
    auto *dialog = new GifPickerDialog(channel, searchTerm, parent);
    dialogs.append(dialog);
    dialog->showAndMoveTo(
        QCursor::pos() - QPoint(dialog->width() / 2, dialog->height() / 2),
        widgets::BoundsChecking::DesiredPosition);
    dialog->search_->setFocus();
}

void GifPickerDialog::showSearch(const QString &searchTerm)
{
    const QSignalBlocker tabsBlocker(this->tabs_);
    const QSignalBlocker searchBlocker(this->search_);
    this->tabs_->setCurrentIndex(0);
    this->search_->setVisible(true);
    this->search_->setText(searchTerm);
    this->resetSearch();
    this->loadPage();
}

void GifPickerDialog::scaleChangedEvent(float scale)
{
    DraggablePopup::scaleChangedEvent(scale);
    this->setMinimumSize(int(330 * scale), int(420 * scale));
    if (this->grid_)
    {
        this->grid_->verticalScrollBar()->setSingleStep(qRound(16 * scale));
        this->layoutWidth_ = -1;
        this->themeChangedEvent();
        this->viewportUpdate_.start(0);
    }
}

GifPickerDialog::~GifPickerDialog()
{
    this->releaseImages();
}

bool GifPickerDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == this)
    {
        if (event->type() == QEvent::WindowDeactivate ||
            event->type() == QEvent::Hide)
        {
            this->updateHover(-1);
        }
        if (event->type() == QEvent::Hide ||
            (event->type() == QEvent::WindowStateChange && this->isMinimized()))
        {
            this->releaseImages();
        }
        if (event->type() == QEvent::Show ||
            event->type() == QEvent::WindowActivate ||
            event->type() == QEvent::WindowStateChange)
        {
            this->viewportUpdate_.start(0);
        }
    }
    else if (watched == this->caption_ && event->type() == QEvent::Resize)
    {
        this->updateCaption();
    }
    else if (watched == this->grid_->viewport())
    {
        if (event->type() == QEvent::MouseMove)
        {
            const auto position =
                static_cast<QMouseEvent *>(event)->position().toPoint();
            this->updateHover(this->isActiveWindow()
                                  ? this->grid_->indexAt(position).row()
                                  : -1);
        }
        else if (event->type() == QEvent::Leave)
        {
            this->updateHover(-1);
        }
        else if (event->type() == QEvent::Resize)
        {
            this->viewportUpdate_.start(0);
        }
    }
    return DraggablePopup::eventFilter(watched, event);
}

void GifPickerDialog::releaseImages()
{
    this->updateHover(-1);
    this->thumbnails_.clear();
}

void GifPickerDialog::updateHover(int row)
{
    if (!getSettings()->animateEmotes || row < 0 || row >= this->gifs_.size() ||
        this->gifs_.at(row).url.isEmpty())
    {
        row = -1;
    }
    if (row == this->hoveredRow_)
    {
        return;
    }
    this->hoverImage_.reset();
    const auto previous = this->hoveredRow_;
    this->hoveredRow_ = row;
    if (row >= 0)
    {
        this->hoverImage_ =
            Image::fromUrl(giphyMedia(this->gifs_.at(row).id, "200w.webp"), 1.0,
                           QSize(200, 200));
    }
    for (const int index : {previous, row})
    {
        if (index >= 0 && index < this->grid_->count())
        {
            this->grid_->viewport()->update(
                this->grid_->visualItemRect(this->grid_->item(index)));
        }
    }
}

void GifPickerDialog::updateViewport()
{
    if (!this->isVisible() || this->isMinimized())
    {
        return;
    }
    const int width = this->grid_->viewport()->width();
    if (this->layoutWidth_ != width || this->layoutCount_ != this->gifs_.size())
    {
        this->layoutWidth_ = width;
        this->layoutCount_ = int(this->gifs_.size());
        this->columns_ = std::max(1, qRound(width / (180 * this->scale())));
        this->rowTops_.clear();
        int top = 0;
        for (int first = 0; first < this->layoutCount_; first += this->columns_)
        {
            const int count =
                std::min(this->columns_, this->layoutCount_ - first);
            double sum = 0;
            for (int i = first; i < first + count; ++i)
            {
                sum += std::clamp(this->gifs_.at(i).aspectRatio, 0.33, 3.0);
            }

            sum *= double(this->columns_) / count;
            const int height =
                qRound(std::clamp((width - 1 - 2 * this->columns_) / sum,
                                  80.0 * this->scale(),
                                  300.0 * this->scale())) +
                2;
            this->rowTops_.append(top);
            top += height;
            double used = 0;
            for (int i = first; i < first + count; ++i)
            {
                const double next =
                    used +
                    (width - 1 - 2 * this->columns_) *
                        std::clamp(this->gifs_.at(i).aspectRatio, 0.33, 3.0) /
                        sum;
                this->grid_->item(i)->setSizeHint(QSize(
                    std::max(3, qRound(next) - qRound(used) + 2), height));
                used = next;
            }
        }
        static_cast<GifListWidget *>(this->grid_)->relayoutItems();
    }
    auto *scroll = this->grid_->verticalScrollBar();
    const auto firstRow = std::max<qsizetype>(
        0, std::upper_bound(this->rowTops_.begin(), this->rowTops_.end(),
                            scroll->value()) -
               this->rowTops_.begin() - 1);
    const auto lastRow =
        std::lower_bound(this->rowTops_.begin(), this->rowTops_.end(),
                         scroll->value() + this->grid_->viewport()->height()) -
        this->rowTops_.begin();
    const int first = int(firstRow) * this->columns_;
    const int last =
        std::min(int(this->gifs_.size()), int(lastRow) * this->columns_);
    this->thumbnails_.removeIf([first, last](const auto &entry) {
        return entry.key() < first || entry.key() >= last;
    });
    for (int row = first; row < last; ++row)
    {
        const auto &gif = this->gifs_.at(row);
        if (!gif.url.isEmpty() && !this->thumbnails_.contains(row))
        {
            this->thumbnails_.insert(
                row, Image::fromUrl(giphyMedia(gif.id, "200w_s.gif"), 1.0,
                                    QSize(200, 200)));
        }
    }
    const auto position =
        this->grid_->viewport()->mapFromGlobal(QCursor::pos());
    this->updateHover(this->isActiveWindow() &&
                              this->grid_->viewport()->underMouse()
                          ? this->grid_->indexAt(position).row()
                          : -1);
    this->grid_->viewport()->update();

    if (this->retry_->isHidden() &&
        scroll->maximum() - scroll->value() <= qRound(160 * this->scale()))
    {
        this->loadPage();
    }
}

void GifPickerDialog::updateCaption()
{
    const int row = this->grid_->currentRow();
    QString title = this->sendError_;
    if (title.isEmpty() && row >= 0 && row < this->gifs_.size())
    {
        title = this->gifs_.at(row).title;
    }
    this->caption_->setText(this->caption_->fontMetrics().elidedText(
        title, Qt::ElideRight, this->caption_->width()));
    this->caption_->setToolTip(title.toHtmlEscaped());
    this->caption_->setAccessibleName(title);
}

void GifPickerDialog::setStatus(const QString &text)
{
    this->status_->setText(text);
    this->status_->setVisible(!text.isEmpty());
}

void GifPickerDialog::refreshAccount()
{
    QString error;
    const auto auth = MoltorinoAuth::resolveSelectedUserToken(&error);
    if ((this->configLoading_ || !this->config_.apiKey.isEmpty()) &&
        auth.token == this->auth_.token && auth.userId == this->auth_.userId &&
        auth.legacy == this->auth_.legacy)
    {
        return;
    }
    const auto generation = ++this->accountGeneration_;
    this->auth_ = auth;
    this->config_ = {};
    this->resetSearch();
    this->retry_->hide();
    this->configLoading_ = false;
    if (!this->auth_.hasToken() || this->auth_.legacy)
    {
        this->setStatus(error.isEmpty()
                            ? "Sign in with Twitch device login in Settings > "
                              "Moltorino to send GIFs with the selected "
                              "account."
                            : error);
        return;
    }
    if (this->channel_->roomId().isEmpty())
    {
        this->setStatus(
            "This channel is still connecting. Try again in a moment.");
        this->retry_->setText("Try again");
        this->retry_->show();
        return;
    }
    this->configLoading_ = true;
    this->setStatus("Loading GIFs...");
    twitchgifs::loadConfig(
        this->channel_->roomId(), this->auth_.token, this,
        [this, generation](auto config) {
            if (generation != this->accountGeneration_)
            {
                return;
            }
            this->configLoading_ = false;
            this->config_ = std::move(config);
            this->loadPage();
        },
        [this, generation](const auto &error) {
            if (generation != this->accountGeneration_)
            {
                return;
            }
            this->configLoading_ = false;
            this->setStatus(error);
            this->retry_->setText("Try again");
            this->retry_->setEnabled(true);
            this->retry_->show();
        });
}

void GifPickerDialog::resetSearch()
{
    ++this->generation_;
    this->loading_ = false;
    this->debounce_.stop();
    this->nextOffset_ = 0;
    this->releaseImages();
    this->layoutCount_ = -1;
    this->gifs_.clear();
    this->grid_->clear();
    this->selectGif();
    if (!this->config_.apiKey.isEmpty() || this->configLoading_)
    {
        this->retry_->hide();
    }
    this->searchTerm_ = this->tabs_->currentIndex() == 0
                            ? this->search_->text().trimmed()
                            : QString{};
}

void GifPickerDialog::loadPage()
{
    if (this->loading_ || this->configLoading_ ||
        this->config_.apiKey.isEmpty() || this->debounce_.isActive() ||
        this->nextOffset_ < 0 || this->gifs_.size() >= 5000)
    {
        return;
    }
    const auto ids = this->tabs_->currentIndex() == 1
                         ? twitchgifs::favoriteIds(
                               getSettings()->favoriteTwitchGifs.getValue())
                         : QStringList{};
    if (this->tabs_->currentIndex() == 1 && ids.isEmpty())
    {
        this->setStatus("No favorites yet. Select a GIF and click Favorite to "
                        "save it here.");
        return;
    }
    this->loading_ = true;
    this->retry_->hide();
    this->setStatus("Loading GIFs...");
    const auto generation = this->generation_;
    twitchgifs::loadPage(
        this->config_, this->searchTerm_, ids, this->nextOffset_, this,
        [this, generation](auto page) {
            if (generation != this->generation_)
            {
                return;
            }
            this->loading_ = false;
            const auto previousSize = this->gifs_.size();
            for (const auto &gif : page.gifs)
            {
                if (this->gifs_.size() == 5000)
                {
                    break;
                }
                if (std::ranges::any_of(this->gifs_, [&](const auto &other) {
                        return other.id == gif.id;
                    }))
                {
                    continue;
                }
                this->gifs_.push_back(gif);
                auto *item = new QListWidgetItem(gif.title, this->grid_);
                item->setData(Qt::AccessibleTextRole, gif.title);
                item->setToolTip("<span>" + gif.title.toHtmlEscaped() +
                                 "</span>");
            }
            this->nextOffset_ = page.nextOffset;

            if (this->gifs_.size() == previousSize)
            {
                this->nextOffset_ = -1;
            }
            this->setStatus(this->gifs_.isEmpty()
                                ? "No GIFs found. Try another search."
                                : QString{});
            this->viewportUpdate_.start(0);
        },
        [this, generation](const auto &error) {
            if (generation != this->generation_)
            {
                return;
            }
            this->loading_ = false;
            this->setStatus(error);
            this->retry_->setText("Try again");
            this->retry_->setEnabled(true);
            this->retry_->show();
        });
}

void GifPickerDialog::selectGif()
{
    this->sendError_.clear();
    const auto row = this->grid_->currentRow();
    const bool selected = row >= 0 && row < this->gifs_.size();
    this->favorite_->setEnabled(selected);
    this->updateCaption();
    const bool saved =
        selected &&
        twitchgifs::favoriteIds(getSettings()->favoriteTwitchGifs.getValue())
            .contains(this->gifs_.at(row).id);
    this->favorite_->setText(saved ? "Unfavorite" : "Favorite");
    this->refreshSend();
}

void GifPickerDialog::toggleFavorite()
{
    const auto row = this->grid_->currentRow();
    if (row < 0 || row >= this->gifs_.size())
    {
        return;
    }
    auto ids =
        twitchgifs::favoriteIds(getSettings()->favoriteTwitchGifs.getValue());
    const auto id = this->gifs_.at(row).id;
    if (ids.contains(id))
    {
        ids.removeAll(id);
    }
    else if (ids.size() < twitchgifs::MAX_FAVORITES)
    {
        ids.prepend(id);
    }
    else
    {
        this->setStatus(
            "Your favorites are full. Remove one before adding another.");
        return;
    }
    getSettings()->favoriteTwitchGifs =
        QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(ids))
                              .toJson(QJsonDocument::Compact));
    getSettings()->requestSave();
    this->selectGif();
    if (this->tabs_->currentIndex() == 1)
    {
        this->resetSearch();
        this->loadPage();
    }
}

void GifPickerDialog::refreshSend()
{
    pruneSends();
    const auto state = sends.value(sendKey(this->auth_, *this->channel_));
    const auto seconds = (state.cooldown.remainingTime() + 999) / 1000;
    const auto row = this->grid_->currentRow();
    const bool selected = row >= 0 && row < this->gifs_.size() &&
                          twitchgifs::validGif(this->gifs_.at(row)) &&
                          !this->config_.apiKey.isEmpty();
    this->send_->setEnabled(selected && !state.pending && seconds == 0);
    this->send_->setText(seconds > 0 ? QString("Wait %1s").arg(seconds)
                                     : "Send");
    if (state.pending || seconds > 0)
    {
        this->cooldownTimer_.start();
    }
    else
    {
        this->cooldownTimer_.stop();
    }
}

void GifPickerDialog::showSendError(const QString &message)
{
    this->sendError_ = message;
    this->updateCaption();
}

void GifPickerDialog::explainSendRejection(const QString &rejectionCode)
{
    const auto token = this->auth_.token;
    const QPointer<GifPickerDialog> weak(this);
    auto channel = this->channel_;
    twitchgifs::loadOwnSubscriptionTier(
        this->channel_->roomId(), token, qApp,
        [weak, token, channel, rejectionCode](int tier) {
            const auto message =
                subscriptionRejectionMessage(rejectionCode, tier);
            if (weak && weak->auth_.token == token)
            {
                weak->showSendError(message);
            }
            else
            {
                channel->addSystemMessage(message);
            }
        },
        [weak, token, channel, rejectionCode](const QString &) {
            const auto message = subscriptionLookupFailedMessage(rejectionCode);
            if (weak && weak->auth_.token == token)
            {
                weak->showSendError(message);
            }
            else
            {
                channel->addSystemMessage(message);
            }
        });
}

void GifPickerDialog::sendGif()
{
    const auto current = MoltorinoAuth::resolveSelectedUserToken();
    if (current.token != this->auth_.token ||
        current.userId != this->auth_.userId ||
        current.legacy != this->auth_.legacy)
    {
        this->refreshAccount();
        return;
    }
    this->refreshSend();
    const auto row = this->grid_->currentRow();
    if (!this->send_->isEnabled() || row < 0 || row >= this->gifs_.size())
    {
        return;
    }
    const auto key = sendKey(this->auth_, *this->channel_);
    sends[key].pending = true;
    this->refreshSend();
    const QPointer<GifPickerDialog> weak(this);
    const auto token = this->auth_.token;

    twitchgifs::send(
        this->channel_->roomId(), token, this->gifs_.at(row), this->searchTerm_,
        qApp, [weak, key, token, channel = this->channel_](auto result) {
            sends[key] = {
                false, QDeadlineTimer(qint64(result.cooldownSeconds) * 1000)};
            const bool sameAccount = weak && weak->auth_.token == token;
            if (sameAccount)
            {
                weak->refreshSend();
            }
            if (result.sent)
            {
                if (sameAccount)
                {
                    weak->close();
                }
                return;
            }
            if (!result.rejectionCode.isEmpty())
            {
                if (sameAccount)
                {
                    weak->explainSendRejection(result.rejectionCode);
                }
                else
                {
                    twitchgifs::loadOwnSubscriptionTier(
                        channel->roomId(), token, qApp,
                        [channel, code = result.rejectionCode](int tier) {
                            channel->addSystemMessage(
                                subscriptionRejectionMessage(code, tier));
                        },
                        [channel,
                         code = result.rejectionCode](const QString &) {
                            channel->addSystemMessage(
                                subscriptionLookupFailedMessage(code));
                        });
                }
                return;
            }
            if (sameAccount)
            {
                weak->showSendError(result.error);
            }
            else if (!result.error.isEmpty())
            {
                channel->addSystemMessage(result.error);
            }
        });
}

}  // namespace chatterino

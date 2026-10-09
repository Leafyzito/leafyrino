#include "widgets/helper/ChannelCategoryPicker.hpp"

#include <QAbstractItemView>
#include <QCompleter>
#include <QEvent>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QPointer>
#include <QStandardItemModel>

#include <algorithm>

namespace chatterino {
namespace {

QString labelFor(const ChannelManagementCategory &category)
{
    return category.displayName.isEmpty() ? category.name
                                          : category.displayName;
}

QString searchKey(const QString &text)
{
    return text.trimmed().toCaseFolded();
}

}  // namespace

ChannelCategoryPicker::ChannelCategoryPicker(QWidget *parent)
    : QLineEdit(parent)
    , completer_(new QCompleter(this))
    , model_(new QStandardItemModel(this))
{
    this->setClearButtonEnabled(true);
    this->setPlaceholderText("Search categories");
    this->setAccessibleName("Category");
    this->completer_->setModel(this->model_);
    this->completer_->setCompletionRole(Qt::DisplayRole);
    this->completer_->setCaseSensitivity(Qt::CaseInsensitive);
    this->completer_->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    this->completer_->setMaxVisibleItems(7);

    this->completer_->setWidget(this);
    this->completer_->popup()->setObjectName(
        "ChannelManagementCategoryResults");
    this->completer_->popup()->installEventFilter(this);
    this->debounce_.setSingleShot(true);
    this->debounce_.setInterval(150);
    this->clock_.start();

    QObject::connect(this, &QLineEdit::textEdited, this,
                     &ChannelCategoryPicker::editQuery);
    QObject::connect(&this->debounce_, &QTimer::timeout, this, [this] {
        this->ready_ = true;
        this->search();
    });
    QObject::connect(this->completer_,
                     qOverload<const QModelIndex &>(&QCompleter::activated),
                     this, &ChannelCategoryPicker::chooseIndex);
}

QAbstractItemView *ChannelCategoryPicker::resultsView() const
{
    return this->completer_->popup();
}

void ChannelCategoryPicker::chooseIndex(const QModelIndex &index)
{
    const auto id = index.data(Qt::UserRole).toString();
    const auto result =
        std::find_if(this->results_.cbegin(), this->results_.cend(),
                     [&id](const auto &category) {
                         return category.id == id;
                     });
    if (result != this->results_.cend())
    {
        this->choose(*result);
    }
}

void ChannelCategoryPicker::setCategory(
    const ChannelManagementCategory &category)
{
    this->initial_ = category;
    this->choose(category);
}

void ChannelCategoryPicker::choose(const ChannelManagementCategory &category)
{
    this->chosen_ = category;
    this->selection_ = category;
    this->stopSearch();
    this->setText(labelFor(this->chosen_));
    this->model_->clear();
    Q_EMIT this->statusChanged({}, false);
    Q_EMIT this->selectionChanged();
}

void ChannelCategoryPicker::stopSearch()
{
    this->query_.clear();
    this->ready_ = false;
    this->debounce_.stop();
    this->completer_->popup()->hide();
}

const ChannelManagementCategory &ChannelCategoryPicker::selection() const
{
    return this->selection_;
}

bool ChannelCategoryPicker::hasValidSelection() const
{
    return searchKey(this->text()) == searchKey(labelFor(this->selection_)) &&
           (!this->selection_.id.isEmpty() || this->initial_.id.isEmpty());
}

void ChannelCategoryPicker::editQuery(const QString &text)
{
    this->debounce_.stop();
    this->ready_ = false;
    this->query_ = searchKey(text);
    this->selection_ = {};
    for (const auto &known : {this->chosen_, this->initial_})
    {
        if (!known.id.isEmpty() && this->query_ == searchKey(labelFor(known)))
        {
            this->selection_ = known;
            break;
        }
    }
    Q_EMIT this->selectionChanged();
    if (this->hasValidSelection())
    {
        this->stopSearch();
        this->model_->clear();
        Q_EMIT this->statusChanged({}, false);
        return;
    }

    if (this->query_.size() < 2)
    {
        this->model_->clear();
        this->completer_->popup()->hide();
        Q_EMIT this->statusChanged("2 characters minimum", false);
        return;
    }

    auto matching = this->results_;
    std::erase_if(matching, [this](const auto &category) {
        return !searchKey(labelFor(category)).contains(this->query_) &&
               !searchKey(category.name).contains(this->query_);
    });
    ChannelManagement::rankCategoryResults(matching, this->query_);
    this->showResults(matching);
    const auto cached = std::find_if(
        this->cache_.begin(), this->cache_.end(), [this](const auto &entry) {
            return entry.query == this->query_ &&
                   this->clock_.elapsed() - entry.timestamp < 300000;
        });
    if (cached != this->cache_.end())
    {
        this->showResults(cached->results);
        return;
    }
    Q_EMIT this->statusChanged({}, false);
    this->debounce_.start();
}

void ChannelCategoryPicker::search()
{
    if (!this->ready_ || !this->isEnabled() || this->query_.size() < 2)
    {
        return;
    }
    Q_EMIT this->statusChanged("Searching", false);
    if (this->inFlight_.contains(this->query_) || this->inFlight_.size() >= 2)
    {
        return;
    }
    const auto query = this->query_;
    this->ready_ = false;
    this->inFlight_.insert(query);
    QPointer<ChannelCategoryPicker> self(this);
    ChannelManagement::searchCategories(
        this->text().trimmed(),
        [self, query](std::vector<ChannelManagementCategory> results) {
            if (!self)
            {
                return;
            }
            self->inFlight_.remove(query);
            ChannelManagement::rankCategoryResults(results, query);
            if (results.size() > 20)
            {
                results.resize(20);
            }
            std::erase_if(self->cache_, [&query](const auto &entry) {
                return entry.query == query;
            });
            self->cache_.push_back({query, self->clock_.elapsed(), results});
            if (self->cache_.size() > 20)
            {
                self->cache_.pop_front();
            }
            if (self->query_ == query)
            {
                self->ready_ = false;
                self->showResults(results);
            }
            self->search();
        },
        [self, query](const QString &error) {
            if (!self)
            {
                return;
            }
            self->inFlight_.remove(query);
            if (self->query_ == query)
            {
                self->ready_ = false;
                Q_EMIT self->statusChanged(error, true);
            }
            self->search();
        });
}

void ChannelCategoryPicker::showResults(
    const std::vector<ChannelManagementCategory> &results)
{
    const auto selected =
        this->completer_->popup()->selectionModel()->selectedIndexes();
    const auto highlighted =
        selected.isEmpty() ? QString()
                           : selected.front().data(Qt::UserRole).toString();
    this->results_ = results;

    if (this->model_->rowCount() > int(this->results_.size()))
    {
        this->model_->removeRows(
            int(this->results_.size()),
            this->model_->rowCount() - int(this->results_.size()));
    }
    int selectedRow = -1;
    int row = 0;
    for (const auto &category : this->results_)
    {
        if (category.id == highlighted)
        {
            selectedRow = row;
        }
        auto *item = this->model_->item(row);
        if (!item)
        {
            item = new QStandardItem;
            this->model_->appendRow(item);
        }
        item->setText(labelFor(category));
        item->setData(category.id, Qt::UserRole);
        item->setToolTip(labelFor(category));
        ++row;
    }
    this->completer_->setCompletionPrefix(this->text());
    if (!results.empty() && this->hasFocus() && this->isVisible() &&
        this->isEnabled())
    {
        this->completer_->complete();
        if (selectedRow >= 0)
        {
            this->completer_->popup()->setCurrentIndex(
                this->completer_->completionModel()->index(selectedRow, 0));
        }
    }
    else
    {
        this->completer_->popup()->hide();
    }
    Q_EMIT this->statusChanged(
        results.empty() ? "No categories found" : QString(), false);
}

void ChannelCategoryPicker::restoreSelection()
{
    this->choose(this->chosen_);
    this->setFocus();
}

void ChannelCategoryPicker::changeEvent(QEvent *event)
{
    QLineEdit::changeEvent(event);
    if (event->type() == QEvent::EnabledChange)
    {
        if (this->isEnabled())
        {
            this->search();
        }
        else
        {
            this->completer_->popup()->hide();
        }
    }
}

bool ChannelCategoryPicker::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == this->completer_->popup() &&
        event->type() == QEvent::KeyPress)
    {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape)
        {
            this->restoreSelection();
            return true;
        }
        if (key->key() == Qt::Key_Tab)
        {
            this->chooseIndex(this->completer_->popup()->currentIndex());
            this->completer_->popup()->hide();
            this->focusNextPrevChild(true);
            return true;
        }
    }
    return QLineEdit::eventFilter(watched, event);
}

void ChannelCategoryPicker::keyPressEvent(QKeyEvent *event)
{
    if (this->completer_->popup()->isVisible() &&
        (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter))
    {
        event->ignore();
        return;
    }
    if (event->key() == Qt::Key_Escape && !this->hasValidSelection())
    {
        this->restoreSelection();
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
         event->key() == Qt::Key_Down) &&
        !this->hasValidSelection())
    {
        this->debounce_.stop();
        if (this->model_->rowCount() > 0)
        {
            this->completer_->complete();
            this->completer_->popup()->setCurrentIndex(
                this->completer_->completionModel()->index(0, 0));
        }
        else
        {
            this->ready_ = true;
            this->search();
        }
        event->accept();
        return;
    }
    QLineEdit::keyPressEvent(event);
}

}  // namespace chatterino

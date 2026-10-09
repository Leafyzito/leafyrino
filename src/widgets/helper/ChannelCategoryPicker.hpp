#pragma once

#include "providers/twitch/ChannelManagement.hpp"

#include <QElapsedTimer>
#include <QLineEdit>
#include <QSet>
#include <QTimer>

#include <deque>

class QCompleter;
class QAbstractItemView;
class QModelIndex;
class QStandardItemModel;

namespace chatterino {

class ChannelCategoryPicker : public QLineEdit
{
    Q_OBJECT

public:
    explicit ChannelCategoryPicker(QWidget *parent = nullptr);

    void setCategory(const ChannelManagementCategory &category);
    void stopSearch();
    const ChannelManagementCategory &selection() const;
    bool hasValidSelection() const;
    QAbstractItemView *resultsView() const;

Q_SIGNALS:
    void selectionChanged();
    void statusChanged(const QString &text, bool error);

protected:
    void changeEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct CachedSearch {
        QString query;
        qint64 timestamp;
        std::vector<ChannelManagementCategory> results;
    };

    void editQuery(const QString &text);
    void search();
    void showResults(const std::vector<ChannelManagementCategory> &results);
    void choose(const ChannelManagementCategory &category);
    void chooseIndex(const QModelIndex &index);
    void restoreSelection();

    QCompleter *completer_;
    QStandardItemModel *model_;
    QTimer debounce_;
    QElapsedTimer clock_;
    std::deque<CachedSearch> cache_;
    QSet<QString> inFlight_;
    QString query_;
    bool ready_ = false;
    ChannelManagementCategory initial_;
    ChannelManagementCategory chosen_;
    ChannelManagementCategory selection_;
    std::vector<ChannelManagementCategory> results_;
};

}

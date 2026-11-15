// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "ForwardDecl.hpp"
#include "widgets/BasePopup.hpp"

#include <QStringList>

#include <memory>

class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QCheckBox;
class QToolButton;
class QVBoxLayout;

namespace chatterino {

class Split;
class MessagePredicate;

class SearchPopup : public BasePopup
{
public:
    SearchPopup(QWidget *parent, Split *split = nullptr);

    virtual void addChannel(ChannelView &channel);
    void goToMessage(const MessagePtr &message);

    void goToMessageId(const QString &messageId);

protected:
    virtual void updateWindowTitle();
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;
    void themeChangedEvent() override;

private:
    void initLayout();
    void search();
    void updateResultCount(size_t matches, size_t total);
    void layoutResultCountLabel();
    void updateResultCountLabelStyle();
    void addShortcuts() override;
    std::vector<MessagePtr> buildSnapshot();

    QScrollArea *createFilterPanel();
    void setFilterPanelOpen(bool open);
    void updateChannelFilters();
    void syncFilterPanel();
    QStringList activeFilterValues(const QString &name) const;
    void setFilterValue(const QString &name, const QString &value,
                        bool enabled);
    void setFilterValues(const QString &name, const QStringList &values);
    bool hasFilters() const;
    void clearFilters();
    void setSearchText(const QString &text);

    static ChannelPtr filter(const QString &text, const QString &channelName,
                             const std::vector<MessagePtr> &snapshot);

    static std::vector<std::unique_ptr<MessagePredicate>> parsePredicates(
        const QString &input);

    std::vector<MessagePtr> snapshot_;
    QLineEdit *searchInput_{};
    QLabel *resultCountLabel_{};
    QPushButton *filterButton_{};

    struct FilterCheckBox {
        QCheckBox *checkBox;
        QString name;
        QString value;
    };
    struct FilterSection {
        QToolButton *header;
        QWidget *body;
    };
    QScrollArea *filterPanel_{};
    std::vector<FilterCheckBox> filterCheckBoxes_;
    std::vector<FilterSection> filterSections_;
    QPushButton *clearFiltersButton_{};
    std::vector<FilterSection> filterMoreGroups_;
    QLineEdit *fromUsersInput_{};
    QWidget *channelSection_{};
    QVBoxLayout *channelFiltersLayout_{};
    ChannelView *channelView_{};
    QString channelName_{};
    Split *split_ = nullptr;
    QList<std::reference_wrapper<ChannelView>> searchChannels_;
};

}  // namespace chatterino

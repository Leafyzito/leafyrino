// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/listview/GenericItemDelegate.hpp"

#include <QListView>

namespace chatterino {

class GenericListModel;
class Theme;

class GenericListView : public QListView
{
    Q_OBJECT

public:
    GenericListView();

    void setModel(QAbstractItemModel *model) override;
    void setModel(GenericListModel *);
    void setInvokeActionOnTab(bool);
    bool eventFilter(QObject *watched, QEvent *event) override;

    GenericListModel *model_{};
    SwitcherItemDelegate itemDelegate_;

    void refreshTheme(const Theme &theme);

Q_SIGNALS:
    void closeRequested();

private:
    bool invokeActionOnTab_{};

    bool acceptCompletion();

    void focusNextCompletion();

    void focusPreviousCompletion();

    void requestClose();
};

}  // namespace chatterino

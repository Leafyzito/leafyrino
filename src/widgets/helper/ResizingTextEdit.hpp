// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <pajlada/signals/signal.hpp>
#include <QCompleter>
#include <QKeyEvent>
#include <QTextEdit>

namespace chatterino {

class ResizingTextEdit : public QTextEdit
{
public:
    ResizingTextEdit();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    bool hasHeightForWidth() const override;
    bool isFirstWord() const;

    pajlada::Signals::Signal<QKeyEvent *> keyPressed;
    pajlada::Signals::NoArgSignal focused;
    pajlada::Signals::NoArgSignal focusLost;
    pajlada::Signals::Signal<const QMimeData *> imagePasted;
    pajlada::Signals::Signal<QMenu *, QPoint> contextMenuRequested;

    void setCompleter(QCompleter *c);

    void resetCompletion();

protected:
    int heightForWidth(int) const override;
    void keyPressEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;

    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

    bool canInsertFromMimeData(const QMimeData *source) const override;
    void insertFromMimeData(const QMimeData *source) override;

    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    qreal documentHeightForWidth(int width) const;
    void invalidateAncestorLayouts();

    QString textUnderCursor(bool *hadSpace = nullptr) const;

    QCompleter *completer_ = nullptr;

    bool completionInProgress_ = false;

    bool eventFilter(QObject *obj, QEvent *event) override;

private Q_SLOTS:
    void insertCompletion(const QString &completion);
};

}  // namespace chatterino

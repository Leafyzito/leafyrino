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
    /// Ignore this prefix when determining the first word for completion.
    void setIgnoredCompletionPrefix(const QString &prefix);

    pajlada::Signals::Signal<QKeyEvent *> keyPressed;
    pajlada::Signals::NoArgSignal focused;
    pajlada::Signals::NoArgSignal focusLost;
    pajlada::Signals::Signal<const QMimeData *> imagePasted;
    pajlada::Signals::Signal<QMenu *, QPoint> contextMenuRequested;

    void setCompleter(QCompleter *c);
    void resetCompletion();
    void setGhostText(QString text);
    const QString &ghostText() const;

protected:
    int heightForWidth(int) const override;
    void keyPressEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;

    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

    bool canInsertFromMimeData(const QMimeData *source) const override;
    void insertFromMimeData(const QMimeData *source) override;

    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    qreal documentHeightForWidth(int width) const;
    void invalidateAncestorLayouts();

    QString textUnderCursor(bool *hadSpace = nullptr) const;

    QCompleter *completer_ = nullptr;
    QString ignoredCompletionPrefix_;
    QString ghostText_;
    /**
     * This is true if a completion was done but the user didn't type yet,
     * and might want to press `Tab` again to get the next completion
     * on the original text.
     *
     * For example:
     *
     * input: "pog"
     * `Tab` pressed:
     *   - complete to "PogBones"
     *   - retain "pog" for next completion
     *   - set `completionInProgress_ = true`
     * `Tab` pressed again:
     *   - complete ["pog"] to "PogChamp"
     *
     * [other key] pressed or cursor moved - updating the input text:
     *   - set `completionInProgress_ = false`
     */
    bool completionInProgress_ = false;

    bool eventFilter(QObject *obj, QEvent *event) override;

private Q_SLOTS:
    void insertCompletion(const QString &completion);
};

}  // namespace chatterino

// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QRegularExpression>
#include <QString>
#include <QSyntaxHighlighter>

#include <concepts>
#include <memory>

class QTextDocument;

namespace chatterino {

class Channel;
class TwitchChannel;
class KickChannel;
class SpellChecker;

namespace inputhighlight::detail {

QRegularExpression wordRegex();

}

class InputHighlighter : public QSyntaxHighlighter
{
public:
    InputHighlighter(SpellChecker &spellChecker, QObject *parent);
    ~InputHighlighter() override;
    InputHighlighter(const InputHighlighter &) = delete;
    InputHighlighter(InputHighlighter &&) = delete;
    InputHighlighter &operator=(const InputHighlighter &) = delete;
    InputHighlighter &operator=(InputHighlighter &&) = delete;

    void setChannel(const std::shared_ptr<Channel> &channel);

    std::vector<QString> getSpellCheckedWords(const QString &text);

    QStringView getWordAt(QStringView text, qsizetype pos);

protected:
    void highlightBlock(const QString &text) override;

private:

    void visitWords(
        const QString &text,
        std::invocable< const QString &, qsizetype,
                       qsizetype> auto &&cb);

    SpellChecker &spellChecker;
    QTextCharFormat spellFmt;

    std::weak_ptr<TwitchChannel> channel;
    std::weak_ptr<KickChannel> kickChannel;

    QRegularExpression wordRegex;
    QRegularExpression tokenRegex;
};

}

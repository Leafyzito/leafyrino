// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/buttons/LabelButton.hpp"

namespace chatterino {

QString LabelButton::mnemonicToDisplayText(const QString &text)
{
    int i = 0;
    QString result;
    result.reserve(text.size());
    while (i < text.size())
    {
        if (text.at(i) == u'&')
        {
            if (i + 1 < text.size() && text.at(i + 1) == u'&')
            {
                result += u'&';
                i += 2;
                continue;
            }
            if (i + 1 < text.size())
            {
                QChar mnemonic = text.at(i + 1);
                result +=
                    QStringLiteral("<u>") + mnemonic + QStringLiteral("</u>");
                i += 2;
                continue;
            }
        }
        result += text.at(i);
        ++i;
    }
    return result;
}

LabelButton::LabelButton(const QString &text, BaseWidget *parent, QSize padding)
    : Button(parent)
    , layout_(this)
    , label_()
    , padding_(padding)
    , text_(text)
{
    this->layout_.setContentsMargins(0, 0, 0, 0);
    this->layout_.addWidget(&this->label_);
    this->label_.setAttribute(Qt::WA_TransparentForMouseEvents);
    this->label_.setAlignment(Qt::AlignCenter);

    if (text.contains(u'&'))
    {
        this->label_.setTextFormat(Qt::RichText);
        this->label_.setText(mnemonicToDisplayText(text));
    }
    else
    {
        this->label_.setText(text);
    }

    this->syncLabelFont();
    this->updatePadding();
}

QString LabelButton::text() const
{
    return this->text_;
}

void LabelButton::setText(const QString &text)
{
    if (this->text_ == text)
    {
        return;
    }
    this->text_ = text;
    if (text.contains(u'&'))
    {
        this->label_.setTextFormat(Qt::RichText);
        this->label_.setText(mnemonicToDisplayText(text));
    }
    else
    {
        this->label_.setTextFormat(Qt::PlainText);
        this->label_.setText(text);
    }
}

QSize LabelButton::padding() const noexcept
{
    return this->padding_;
}

void LabelButton::setPadding(QSize padding)
{
    if (this->padding_ == padding)
    {
        return;
    }

    this->padding_ = padding;
    this->updatePadding();
}

void LabelButton::enableRichText()
{
    this->label_.setTextFormat(Qt::RichText);
}

void LabelButton::changeEvent(QEvent *event)
{
    Button::changeEvent(event);

    if (event->type() == QEvent::FontChange)
    {
        this->syncLabelFont();
    }
}

void LabelButton::paintContent(QPainter &painter)
{
}

void LabelButton::syncLabelFont()
{
    this->label_.setFont(this->font());
}

void LabelButton::updatePadding()
{
    auto x = this->padding_.width();
    auto y = this->padding_.height();
    this->label_.setContentsMargins(x, y, x, y);
}

}

// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/buttons/LabelButton.hpp"

namespace chatterino {

LabelButton::LabelButton(const QString &text, BaseWidget *parent, QSize padding)
    : Button(parent)
    , layout_(this)
    , label_(text)
    , padding_(padding)
{
    this->layout_.setContentsMargins(0, 0, 0, 0);
    this->layout_.addWidget(&this->label_);
    this->label_.setAttribute(Qt::WA_TransparentForMouseEvents);
    this->label_.setAlignment(Qt::AlignCenter);

    this->syncLabelFont();
    this->updatePadding();
}

QString LabelButton::text() const
{
    return this->label_.text();
}

void LabelButton::setText(const QString &text)
{
    this->label_.setText(text);
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

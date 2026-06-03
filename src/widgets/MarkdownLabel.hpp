// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/Label.hpp"

class QTextDocument;

namespace chatterino {

class MarkdownLabel : public Label
{
public:
    explicit MarkdownLabel(BaseWidget *parent, QString text,
                           FontStyle style = FontStyle::UiMedium);

    void setText(const QString &text);

protected:
    void paintEvent(QPaintEvent * ) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    void updateSize() override;

    QTextDocument *markdownDocument;
};

}

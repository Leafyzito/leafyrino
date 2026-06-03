// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/buttons/Button.hpp"

#include <QHBoxLayout>
#include <QLabel>

namespace chatterino {

class LabelButton : public Button
{
public:
    LabelButton(const QString &text = {}, BaseWidget *parent = nullptr,
                QSize padding = {6, 0});

    [[nodiscard]] QString text() const;

    void setText(const QString &text);

    [[nodiscard]] QSize padding() const noexcept;

    void setPadding(QSize padding);

    void enableRichText();

protected:
    void changeEvent(QEvent *event) override;
    void paintContent(QPainter &painter) override;

private:
    void syncLabelFont();
    void updatePadding();

    QHBoxLayout layout_;
    QLabel label_;
    QSize padding_;
};

}

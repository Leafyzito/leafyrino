// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "singletons/Fonts.hpp"
#include "widgets/BaseWidget.hpp"

#include <pajlada/signals/signalholder.hpp>

class QFontMetricsF;

namespace chatterino {

class Label : public BaseWidget
{
public:
    explicit Label(QString text = QString(),
                   FontStyle style = FontStyle::UiMedium);
    explicit Label(BaseWidget *parent, QString text = QString(),
                   FontStyle style = FontStyle::UiMedium);

    const QString &getText() const;
    void setText(const QString &text);

    FontStyle getFontStyle() const;
    void setFontStyle(FontStyle style);

    bool getCentered() const;
    void setCentered(bool centered);

    void setPadding(QMargins padding);

    bool getWordWrap() const;
    void setWordWrap(bool wrap);

    void setShouldElide(bool shouldElide);

protected:
    void scaleChangedEvent(float scale_) override;
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *event) override;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    virtual void updateSize();
    QRectF textRect() const;

    QFontMetricsF getFontMetrics() const;

    bool updateElidedText(const QFontMetricsF &fontMetrics, qreal width);

    QString text_;
    FontStyle fontStyle_;
    QSize sizeHint_;
    QSize minimumSizeHint_;

    QMargins basePadding_;

    QMarginsF currentPadding_;

    bool centered_ = false;
    bool wordWrap_ = false;
    bool shouldElide_ = false;

    QString elidedText_;

    pajlada::Signals::SignalHolder connections_;
};

}

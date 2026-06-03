// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWidget.hpp"

#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPoint>
#include <QTimer>
#include <QWidget>

#include <optional>

namespace chatterino {

class Button : public BaseWidget
{
    Q_OBJECT

    struct ClickEffect {
        double progress = 0.0;
        QPoint position;

        ClickEffect(QPoint _position)
            : position(_position)
        {
        }
    };

public:
    Button(BaseWidget *parent = nullptr);

    [[nodiscard]] bool enabled() const noexcept;

    void setEnabled(bool enabled);

    [[nodiscard]] bool mouseOver() const noexcept;

    [[nodiscard]] bool leftMouseButtonDown() const noexcept;

    [[nodiscard]] bool menuVisible() const noexcept;

    [[nodiscard]] QColor borderColor() const noexcept;

    void setBorderColor(const QColor &color);

    [[nodiscard]] std::optional<QColor> mouseEffectColor() const;

    void setMouseEffectColor(std::optional<QColor> color);

    [[nodiscard]] QMenu *menu() const;

    void setMenu(std::unique_ptr<QMenu> menu);

    void enableDrops(const std::vector<QString> &acceptedDropMimes_);

Q_SIGNALS:

    void dropEvent(QDropEvent *event) override;

    void leftClicked();

    void clicked(Qt::MouseButton button);

    void leftMousePress();

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;

    void paintEvent(QPaintEvent *) override;

    virtual void paintContent(QPainter &painter) = 0;

    void fancyPaint(QPainter &painter);

    void invalidateContent();

    [[nodiscard]] bool contentCacheEnabled() const noexcept;

    void setContentCacheEnabled(bool enabled);

    [[nodiscard]] bool opaqueContent() const noexcept;

    void setOpaqueContent(bool opaqueContent);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    void enterEvent(QEnterEvent *) override;
#else
    void enterEvent(QEvent *) override;
#endif
    void leaveEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

    void addClickEffect(QPoint position);

    virtual void mouseOverUpdated()
    {
    }

private:
    void onMouseEffectTimeout();
    void showMenu();

    void paintButton(QPainter &painter);

    QColor borderColor_;

    QPoint mousePos_;
    double hoverMultiplier_ = 0.0;

    std::unique_ptr<QMenu> menu_;

    QTimer effectTimer_;
    std::vector<ClickEffect> clickEffects_;
    std::optional<QColor> mouseEffectColor_;

    bool enabled_ = true;
    bool mouseOver_ = false;
    bool leftMouseButtonDown_ = false;
    bool rightMouseButtonDown_ = false;
    bool middleMouseButtonDown_ = false;
    bool menuVisible_ = false;

    QPixmap cachedPixmap_;
    bool pixmapValid_ = false;
    bool cachePixmap_ = false;
    bool opaqueContent_ = false;

    std::vector<QString> acceptedDropMimes;
};

}  // namespace chatterino

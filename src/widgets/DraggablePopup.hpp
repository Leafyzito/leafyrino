// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "buttons/SvgButton.hpp"
#include "widgets/BaseWindow.hpp"

#include <QPoint>
#include <QTimer>

#include <memory>

namespace chatterino {

class DraggablePopup : public BaseWindow
{
    Q_OBJECT

public:
    DraggablePopup(bool closeAutomatically, QWidget *parent);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

    Button *createPinButton();

    /// Pins \a parent when it is an auto-closing DraggablePopup so it stays
    /// open while a child popup takes focus.
    ///
    /// Returns true when this call newly pinned the parent.
    static bool pinParentIfNeeded(QWidget *parent);

    /// Unpins \a parent when it is a DraggablePopup. Call only when
    /// pinParentIfNeeded previously returned true for that parent.
    static void unpinParentIfNeeded(QWidget *parent);

    std::shared_ptr<bool> lifetimeHack_;

    void togglePinned();

    bool ensurePinned();

private:
    bool isMoving_ = false;

    bool closeAutomatically_ = false;

    QPoint startPosDrag_;

    QPoint requestedDragPos_;

    QTimer dragTimer_;

    SvgButton *pinButton_{};
    SvgButton::Src pinDisabledSource_{
        .dark = ":/buttons/pinDisabled-darkMode.svg",
        .light = ":/buttons/pinDisabled-lightMode.svg",
    };
    SvgButton::Src pinEnabledSource_{
        .dark = ":/buttons/pinEnabled.svg",
        .light = ":/buttons/pinEnabled.svg",
    };
    bool isPinned_ = false;
};

}  // namespace chatterino

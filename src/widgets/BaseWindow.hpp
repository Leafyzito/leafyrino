// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/FlagsEnum.hpp"
#include "util/WidgetHelpers.hpp"
#include "widgets/BaseWidget.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QTimer>

#include <functional>

class QHBoxLayout;
struct tagMSG;
typedef struct tagMSG MSG;

namespace chatterino {

class Button;
class LabelButton;
class PixmapButton;
class TitleBarButton;
class TitleBarButtons;
enum class TitleBarButtonStyle : std::uint8_t;

class BaseWindow : public BaseWidget
{
    Q_OBJECT

public:
    enum Flags {
        None = 0,
        EnableCustomFrame = 1 << 0,
        Frameless = 1 << 1,
        TopMost = 1 << 2,
        DisableCustomScaling = 1 << 3,
        FramelessDraggable = 1 << 4,
        DontFocus = 1 << 5,
        Dialog = 1 << 6,
        DisableLayoutSave = 1 << 7,
        BoundsCheckOnShow = 1 << 8,
        ClearBuffersOnDpiChange = 1 << 9,

        LinuxPopup = 1 << 10,

        /// Override the default stylesheet & user-specificed theme with our settings.qss stylesheet
        UseSettingsStylesheet = 1 << 11,
    };

    explicit BaseWindow(FlagsEnum<Flags> flags_ = None,
                        QWidget *parent = nullptr);
    ~BaseWindow() override;

    void setInitialBounds(QRect bounds, widgets::BoundsChecking mode);
    QRect getBounds() const;

    QWidget *getLayoutContainer();
    bool hasCustomWindowFrame() const;

    template <typename T>
    T *addTitleBarButton(std::function<void()> onClicked, auto &&...args)
    {
        auto *button = new T(std::forward<decltype(args)>(args)...);
        button->setScaleIndependentSize(30, 30);
        this->appendTitlebarButton(button);

        QObject::connect(button, &T::leftClicked, this, std::move(onClicked));

        return button;
    }

    LabelButton *addTitleBarLabel(std::function<void()> onClicked);

    void moveTo(QPoint point, widgets::BoundsChecking mode);

    void showAndMoveTo(QPoint point, widgets::BoundsChecking mode);

    bool applyLastBoundsCheck();

    float scale() const override;

    bool isTopMost() const;

    void setTopMost(bool topMost);

    pajlada::Signals::NoArgSignal closing;
    pajlada::Signals::NoArgSignal leaving;

    static bool supportsCustomWindowFrame();

Q_SIGNALS:
    void topMostChanged(bool topMost);

protected:
    enum class FocusOutAction : std::uint8_t {
        None,
        Hide,
    };

    FocusOutAction focusOutAction = FocusOutAction::None;

    enum class WindowDeactivateAction : std::uint8_t {
        Nothing,
        Delete,
        Close,
        Hide,
    };

    WindowDeactivateAction windowDeactivateAction =
        WindowDeactivateAction::Nothing;

    virtual void windowDeactivationEvent();

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    bool nativeEvent(const QByteArray &eventType, void *message,
                     qintptr *result) override;
#else
    bool nativeEvent(const QByteArray &eventType, void *message,
                     long *result) override;
#endif
    void scaleChangedEvent(float) override;

    void paintEvent(QPaintEvent *) override;
    virtual void drawOutline(QPainter &);

    void changeEvent(QEvent *) override;
    void leaveEvent(QEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void moveEvent(QMoveEvent *) override;
    void closeEvent(QCloseEvent *) override;
    void showEvent(QShowEvent *) override;

    void themeChangedEvent() override;
    bool event(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

    void focusOutEvent(QFocusEvent *event) override;

    QPointF movingRelativePos;
    bool moving{};

    virtual float desiredScale() const;
    void updateScale();

    std::optional<QColor> overrideBackgroundColor_;

private:
    void init();

    void calcButtonsSizes();
    void drawCustomWindowFrame(QPainter &painter);

    static void applyScaleRecursive(QObject *root, float scale);

    bool handleSHOWWINDOW(MSG *msg);
    bool handleSIZE(MSG *msg);
    bool handleMOVE(MSG *msg);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    bool handleNCCALCSIZE(MSG *msg, qintptr *result);
    bool handleNCHITTEST(MSG *msg, qintptr *result);
#else
    bool handleNCCALCSIZE(MSG *msg, long *result);
    bool handleNCHITTEST(MSG *msg, long *result);
#endif

    void appendTitlebarButton(Button *button);

    bool enableCustomFrame_;
    bool frameless_;
    bool shown_ = false;
    FlagsEnum<Flags> flags_;
    bool isTopMost_ = false;

    struct {
        QLayout *windowLayout = nullptr;
        QHBoxLayout *titlebarBox = nullptr;
        QWidget *titleLabel = nullptr;
        TitleBarButtons *titlebarButtons = nullptr;
        QWidget *layoutBase = nullptr;
        std::vector<Button *> buttons;
    } ui_;

    QPoint lastBoundsCheckPosition_;

    widgets::BoundsChecking lastBoundsCheckMode_ = widgets::BoundsChecking::Off;

#ifdef USEWINSDK
    void updateRealSize();

    std::optional<HWND> safeHWND() const;

    void tryApplyTopMost();
    bool waitingForTopMost_ = false;

    QRect initalBounds_;
    QRect currentBounds_;
    QTimer useNextBounds_;
    bool isNotMinimizedOrMaximized_{};
    bool lastEventWasNcMouseMove_ = false;

    QRect realBounds_;
    bool isMaximized_ = false;
#endif

    pajlada::Signals::SignalHolder connections_;

    friend class BaseWidget;
};

}  // namespace chatterino

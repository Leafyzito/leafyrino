// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/BaseWindow.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/DebugCount.hpp"
#include "util/PostToThread.hpp"
#include "util/WindowsHelper.hpp"
#include "widgets/buttons/LabelButton.hpp"
#include "widgets/buttons/TitlebarButton.hpp"
#include "widgets/buttons/TitlebarButtons.hpp"
#include "widgets/Label.hpp"
#include "widgets/Window.hpp"

#include <QApplication>
#include <QFont>
#include <QIcon>
#include <QScreen>
#include <QWindow>

#include <functional>

#ifdef USEWINSDK
// clang-format off
#    include <Windows.h>
#    include <dwmapi.h>
#    include <shellapi.h>
#    include <VersionHelpers.h>
#    include <windowsx.h>
// clang-format on

#    pragma comment(lib, "Dwmapi.lib")

#    include <QHBoxLayout>
#    include <QMargins>
#    include <QOperatingSystemVersion>
#endif

namespace {

using namespace chatterino;

#ifdef USEWINSDK

constexpr UINT HIDDEN_TASKBAR_SIZE = 2;

bool isWindows11OrGreater()
{
    static const bool result = [] {
        auto version = QOperatingSystemVersion::current();
        return (version.majorVersion() > 10) ||
               (version.microVersion() >= 22000);
    }();

    return result;
}

HWND findTaskbarWindow(LPRECT rcMon = nullptr)
{
    HWND taskbar = nullptr;
    RECT taskbarRect;

    RECT intersectionRect;

    while ((taskbar = FindWindowEx(nullptr, taskbar, L"Shell_TrayWnd",
                                   nullptr)) != nullptr)
    {
        if (!rcMon)
        {
            break;
        }
        if (GetWindowRect(taskbar, &taskbarRect) != 0 &&
            IntersectRect(&intersectionRect, &taskbarRect, rcMon) != 0)
        {
            break;
        }
    }

    return taskbar;
}

std::optional<UINT> hiddenTaskbarEdge(LPRECT rcMon = nullptr)
{
    HWND taskbar = findTaskbarWindow(rcMon);
    if (!taskbar)
    {
        return std::nullopt;
    }

    APPBARDATA state = {sizeof(state), taskbar};
    APPBARDATA pos = {sizeof(pos), taskbar};

    auto appBarState =
        static_cast<LRESULT>(SHAppBarMessage(ABM_GETSTATE, &state));
    if ((appBarState & ABS_AUTOHIDE) == 0)
    {
        return std::nullopt;
    }

    if (SHAppBarMessage(ABM_GETTASKBARPOS, &pos) == 0)
    {
        qCDebug(chatterinoApp) << "Failed to get taskbar pos";
        return ABE_BOTTOM;
    }

    return pos.uEdge;
}

RECT windowBordersFor(HWND hwnd, bool isMaximized)
{
    RECT margins{0, 0, 0, 0};

    auto addBorders = isMaximized || isWindows11OrGreater();
    if (addBorders)
    {
#    if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        auto dpi = GetDpiForWindow(hwnd);
#    endif

        auto systemMetric = [&](auto index) {
#    if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            if (dpi != 0)
            {
                return GetSystemMetricsForDpi(index, dpi);
            }
#    endif
            return GetSystemMetrics(index);
        };

        auto paddedBorder = systemMetric(SM_CXPADDEDBORDER);
        auto borderWidth = systemMetric(SM_CXSIZEFRAME) + paddedBorder;
        auto borderHeight = systemMetric(SM_CYSIZEFRAME) + paddedBorder;

        margins.left += borderWidth;
        margins.right -= borderWidth;
        if (isMaximized)
        {
            margins.top += borderHeight;
        }
        margins.bottom -= borderHeight;
    }

    if (isMaximized)
    {
        auto *hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        auto *monitor = [&]() -> LPRECT {
            if (GetMonitorInfo(hMonitor, &mi))
            {
                return &mi.rcMonitor;
            }
            return nullptr;
        }();

        auto edge = hiddenTaskbarEdge(monitor);
        if (edge)
        {
            switch (*edge)
            {
                case ABE_LEFT:
                    margins.left += HIDDEN_TASKBAR_SIZE;
                    break;
                case ABE_RIGHT:
                    margins.right -= HIDDEN_TASKBAR_SIZE;
                    break;
                case ABE_TOP:
                    margins.top += HIDDEN_TASKBAR_SIZE;
                    break;
                case ABE_BOTTOM:
                    margins.bottom -= HIDDEN_TASKBAR_SIZE;
                    break;
                default:
                    break;
            }
        }
    }

    return margins;
}

#endif

Qt::WindowFlags windowFlagsFor(FlagsEnum<BaseWindow::Flags> flags)
{
    Qt::WindowFlags out;
    if (flags.has(BaseWindow::Dialog))
    {
        out.setFlag(Qt::Dialog);
    }
    else
    {
        out.setFlag(Qt::Window);
    }
    out.setFlag(Qt::WindowStaysOnTopHint, flags.has(BaseWindow::TopMost));
    out.setFlag(Qt::FramelessWindowHint, flags.has(BaseWindow::Frameless));

#ifdef Q_OS_LINUX
    if (flags.has(BaseWindow::LinuxPopup))
    {
        out.setFlag(Qt::Popup);
    }
#endif

    return out;
}

}  // namespace

namespace chatterino {

BaseWindow::BaseWindow(FlagsEnum<Flags> _flags, QWidget *parent)
    : BaseWidget(parent, windowFlagsFor(_flags))
    , enableCustomFrame_(_flags.has(EnableCustomFrame))
    , frameless_(_flags.has(Frameless))
    , flags_(_flags)
{
    if (this->frameless_)
    {
        this->enableCustomFrame_ = false;
    }

    if (_flags.has(DontFocus))
    {
        this->setAttribute(Qt::WA_ShowWithoutActivating);
#ifdef Q_OS_LINUX
        this->setWindowFlags(Qt::ToolTip);
#else
        this->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint |
                             Qt::WindowDoesNotAcceptFocus |
                             Qt::BypassWindowManagerHint);
#endif
    }

    this->init();

    getSettings()->uiScale.connect(
        [this]() {
            postToThread([this] {
                this->updateScale();
            });
        },
        this->connections_, false);

    this->updateScale();

    this->resize(300, 150);

#ifdef USEWINSDK
    this->useNextBounds_.setSingleShot(true);
    QObject::connect(&this->useNextBounds_, &QTimer::timeout, this, [this]() {
        this->currentBounds_ = this->geometry();
    });
#endif

    this->themeChangedEvent();
    DebugCount::increase(DebugObject::BaseWindow);
}

BaseWindow::~BaseWindow()
{
    DebugCount::decrease(DebugObject::BaseWindow);
}

void BaseWindow::setInitialBounds(QRect bounds, widgets::BoundsChecking mode)
{
    bounds = widgets::checkInitialBounds(bounds, mode);
#ifdef USEWINSDK
    this->initalBounds_ = bounds;
#else
    this->setGeometry(bounds);
#endif
}

QRect BaseWindow::getBounds() const
{
#ifdef USEWINSDK
    return this->currentBounds_;
#else
    return this->geometry();
#endif
}

float BaseWindow::scale() const
{
    return std::max<float>(0.01f, this->overrideScale().value_or(this->scale_));
}

void BaseWindow::init()
{
#ifdef USEWINSDK
    if (this->hasCustomWindowFrame())
    {
        auto *layout = new QVBoxLayout(this);
        this->ui_.windowLayout = layout;
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        if (!this->frameless_)
        {
            QHBoxLayout *buttonLayout = this->ui_.titlebarBox =
                new QHBoxLayout();
            buttonLayout->setContentsMargins(0, 0, 0, 0);
            layout->addLayout(buttonLayout);

            Label *title = new Label;
            QObject::connect(this, &QWidget::windowTitleChanged,
                             [title](const QString &text) {
                                 title->setText(text);
                             });

            QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            policy.setHorizontalStretch(1);
            title->setSizePolicy(policy);
            buttonLayout->addWidget(title);
            this->ui_.titleLabel = title;

            auto *minButton = new TitleBarButton;
            minButton->setButtonStyle(TitleBarButtonStyle::Minimize);
            auto *maxButton = new TitleBarButton;
            maxButton->setButtonStyle(TitleBarButtonStyle::Maximize);
            auto *exitButton = new TitleBarButton;
            exitButton->setButtonStyle(TitleBarButtonStyle::Close);

            QObject::connect(minButton, &TitleBarButton::leftClicked, this,
                             [this] {
                                 this->setWindowState(Qt::WindowMinimized |
                                                      this->windowState());
                             });
            QObject::connect(
                maxButton, &TitleBarButton::leftClicked, this,
                [this, maxButton] {
                    this->setWindowState(maxButton->getButtonStyle() !=
                                                 TitleBarButtonStyle::Maximize
                                             ? Qt::WindowActive
                                             : Qt::WindowMaximized);
                });
            QObject::connect(exitButton, &TitleBarButton::leftClicked, this,
                             [this] {
                                 this->close();
                             });

            this->ui_.titlebarButtons =
                new TitleBarButtons(this, minButton, maxButton, exitButton);

            this->ui_.buttons.push_back(minButton);
            this->ui_.buttons.push_back(maxButton);
            this->ui_.buttons.push_back(exitButton);

            buttonLayout->addWidget(minButton);
            buttonLayout->addWidget(maxButton);
            buttonLayout->addWidget(exitButton);
            buttonLayout->setSpacing(0);
        }

        this->ui_.layoutBase = new BaseWidget(this);
        if (isWindows11OrGreater())
        {
            this->ui_.layoutBase->setContentsMargins(0, 0, 0, 0);
        }
        else
        {
            this->ui_.layoutBase->setContentsMargins(1, 0, 1, 1);
        }
        layout->addWidget(this->ui_.layoutBase);
    }
#endif

    if (!this->flags_.has(TopMost))
    {
        getSettings()->windowTopMost.connect(
            [this](bool topMost) {
                this->setTopMost(topMost);
            },
            this->connections_);
    }
}

void BaseWindow::setTopMost(bool topMost)
{
    if (this->flags_.has(TopMost))
    {
        qCWarning(chatterinoWidget)
            << "Called setTopMost on a window with the `TopMost` flag set.";
        return;
    }

    if (this->isTopMost_ == topMost)
    {
        return;
    }
    this->isTopMost_ = topMost;

#ifdef USEWINSDK
    if (!this->waitingForTopMost_)
    {
        this->tryApplyTopMost();
    }
#else
    auto isVisible = this->isVisible();
    this->setWindowFlag(Qt::WindowStaysOnTopHint, topMost);
    if (isVisible)
    {
        this->show();
    }
#endif

    this->topMostChanged(this->isTopMost_);
}

#ifdef USEWINSDK
void BaseWindow::tryApplyTopMost()
{
    auto hwnd = this->safeHWND();
    if (!hwnd)
    {
        this->waitingForTopMost_ = true;
        QTimer::singleShot(50, this, &BaseWindow::tryApplyTopMost);
        return;
    }
    this->waitingForTopMost_ = false;

    if (this->parent())
    {
        return;
    }

    ::SetWindowPos(*hwnd, this->isTopMost_ ? HWND_TOPMOST : HWND_NOTOPMOST, 0,
                   0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
#endif

bool BaseWindow::isTopMost() const
{
    return this->isTopMost_ || this->flags_.has(TopMost);
}

QWidget *BaseWindow::getLayoutContainer()
{
    if (this->hasCustomWindowFrame())
    {
        return this->ui_.layoutBase;
    }
    else
    {
        return this;
    }
}

bool BaseWindow::hasCustomWindowFrame() const
{
    return BaseWindow::supportsCustomWindowFrame() && this->enableCustomFrame_;
}

bool BaseWindow::supportsCustomWindowFrame()
{
#ifdef USEWINSDK
    static bool isWin8 = IsWindows8OrGreater();

    return isWin8;
#else
    return false;
#endif
}

void BaseWindow::windowDeactivationEvent()
{
    switch (this->windowDeactivateAction)
    {
        case WindowDeactivateAction::Delete:
            this->deleteLater();
            break;

        case WindowDeactivateAction::Close:
            this->close();
            break;

        case WindowDeactivateAction::Hide:
            this->hide();
            break;

        case WindowDeactivateAction::Nothing:
        default:
            break;
    }
}

void BaseWindow::themeChangedEvent()
{
    if (this->hasCustomWindowFrame())
    {
        QPalette palette;
        palette.setColor(QPalette::Window, QColor(80, 80, 80, 255));
        palette.setColor(QPalette::WindowText, this->theme->window.text);
        this->setPalette(palette);

        if (this->ui_.titleLabel)
        {
            QPalette palette_title;
            palette_title.setColor(
                QPalette::WindowText,
                this->theme->isLightTheme() ? "#333" : "#ccc");
            this->ui_.titleLabel->setPalette(palette_title);
        }

        for (Button *button : this->ui_.buttons)
        {
            button->setMouseEffectColor(this->theme->window.text);
        }
    }
    else
    {
        QPalette palette;
        palette.setColor(QPalette::Window, this->theme->window.background);
        palette.setColor(QPalette::WindowText, this->theme->window.text);
        this->setPalette(palette);
    }
}

bool BaseWindow::event(QEvent *event)
{
    if (event->type() == QEvent::WindowDeactivate)
    {
        this->windowDeactivationEvent();
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    if (this->flags_.hasAny(DontFocus, Dialog, FramelessDraggable))
    {
        if (event->type() == QEvent::ParentWindowChange)
        {
            assert(this->windowHandle() != nullptr);
            if (this->windowHandle()->parent() != nullptr)
            {
                this->windowHandle()->setParent(nullptr);
            }
        }
    }
#endif

    return QWidget::event(event);
}

void BaseWindow::wheelEvent(QWheelEvent *event)
{
    if (event->angleDelta().x() != 0)
    {
        return;
    }

    if (event->modifiers() & Qt::ControlModifier)
    {
        if (event->angleDelta().y() > 0)
        {
            getSettings()->setClampedUiScale(
                getSettings()->getClampedUiScale() + 0.1);
        }
        else
        {
            getSettings()->setClampedUiScale(
                getSettings()->getClampedUiScale() - 0.1);
        }
    }
}

void BaseWindow::mousePressEvent(QMouseEvent *event)
{
#ifndef Q_OS_WIN
    if (this->flags_.has(FramelessDraggable))
    {
        this->movingRelativePos = event->position();
        auto pos = event->position().toPoint();
        if (auto *widget = this->childAt(pos.x(), pos.y()))
        {
            std::function<bool(QWidget *)> recursiveCheckMouseTracking;
            recursiveCheckMouseTracking = [&](QWidget *widget) {
                if (widget == nullptr || widget->isHidden())
                {
                    return false;
                }

                if (widget->hasMouseTracking())
                {
                    return true;
                }

                return recursiveCheckMouseTracking(widget->parentWidget());
            };

            if (!recursiveCheckMouseTracking(widget) &&
                !this->windowHandle()->startSystemMove())
            {
                this->moving = true;
            }
        }
    }
#endif

    BaseWidget::mousePressEvent(event);
}

void BaseWindow::mouseReleaseEvent(QMouseEvent *event)
{
#ifndef Q_OS_WIN
    if (this->flags_.has(FramelessDraggable))
    {
        if (this->moving)
        {
            this->moving = false;
        }
    }
#endif

    BaseWidget::mouseReleaseEvent(event);
}

void BaseWindow::mouseMoveEvent(QMouseEvent *event)
{
#ifndef Q_OS_WIN
    if (this->flags_.has(FramelessDraggable))
    {
        if (this->moving)
        {
            auto newPos =
                (event->globalPosition() - this->movingRelativePos).toPoint();
            this->move(newPos.x(), newPos.y());
        }
    }
#endif

    BaseWidget::mouseMoveEvent(event);
}

void BaseWindow::focusOutEvent(QFocusEvent *event)
{
    switch (this->focusOutAction)
    {
        case FocusOutAction::Hide:
            this->hide();
            break;

        case FocusOutAction::None:
        default:
            break;
    }

    BaseWidget::focusOutEvent(event);
}

void BaseWindow::appendTitlebarButton(Button *button)
{
    this->ui_.buttons.push_back(button);
    this->ui_.titlebarBox->insertWidget(1, button);
}

LabelButton *BaseWindow::addTitleBarLabel(std::function<void()> onClicked)
{
    auto *button = new LabelButton;
    button->setScaleIndependentHeight(30);

    this->appendTitlebarButton(button);

    QObject::connect(button, &LabelButton::leftClicked, this,
                     std::move(onClicked));

    return button;
}

void BaseWindow::changeEvent(QEvent *)
{
#ifdef USEWINSDK
    if (this->ui_.titlebarButtons)
    {
        this->ui_.titlebarButtons->updateMaxButton();
    }

    if (this->isVisible() && this->hasCustomWindowFrame())
    {
        auto hwnd = this->safeHWND();
        if (hwnd)
        {
            auto palette = this->palette();
            palette.setColor(QPalette::Window, GetForegroundWindow() == *hwnd
                                                   ? QColor(90, 90, 90)
                                                   : QColor(50, 50, 50));
            this->setPalette(palette);
        }
    }
#endif

#ifndef Q_OS_WIN
    this->update();
#endif
}

void BaseWindow::leaveEvent(QEvent *)
{
    this->leaving.invoke();
}

void BaseWindow::moveTo(QPoint point, widgets::BoundsChecking mode)
{
    this->lastBoundsCheckPosition_ = point;
    this->lastBoundsCheckMode_ = mode;
    widgets::moveWindowTo(this, point, mode);
}

void BaseWindow::showAndMoveTo(QPoint point, widgets::BoundsChecking mode)
{
    this->lastBoundsCheckPosition_ = point;
    this->lastBoundsCheckMode_ = mode;
    widgets::showAndMoveWindowTo(this, point, mode);
}

bool BaseWindow::applyLastBoundsCheck()
{
    if (this->lastBoundsCheckMode_ == widgets::BoundsChecking::Off)
    {
        return false;
    }

    this->moveTo(this->lastBoundsCheckPosition_, this->lastBoundsCheckMode_);
    return true;
}

void BaseWindow::resizeEvent(QResizeEvent *)
{
    if (!this->flags_.has(DisableLayoutSave))
    {
        getApp()->getWindows()->queueSave();
    }

#ifdef USEWINSDK
    this->calcButtonsSizes();
    this->updateRealSize();
#endif
}

void BaseWindow::moveEvent(QMoveEvent *event)
{
#ifdef CHATTERINO
    if (!this->flags_.has(DisableLayoutSave))
    {
        getApp()->getWindows()->queueSave();
    }
#endif

    BaseWidget::moveEvent(event);
}

void BaseWindow::closeEvent(QCloseEvent *)
{
    this->closing.invoke();
}

void BaseWindow::showEvent(QShowEvent *)
{
#ifdef Q_OS_WIN
    if (this->flags_.has(BoundsCheckOnShow))
    {
        this->moveTo(this->pos(), widgets::BoundsChecking::CursorPosition);
    }

    if (!this->flags_.has(TopMost))
    {
        QTimer::singleShot(1, this, [this] {
            if (!this->waitingForTopMost_)
            {
                this->tryApplyTopMost();
            }
        });
    }
#endif
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
bool BaseWindow::nativeEvent(const QByteArray &eventType, void *message,
                             qintptr *result)
#else
bool BaseWindow::nativeEvent(const QByteArray &eventType, void *message,
                             long *result)
#endif
{
#ifdef USEWINSDK
    MSG *msg = reinterpret_cast<MSG *>(message);

    bool returnValue = false;

    auto isHoveringTitlebarButton = [&]() {
        auto ht = msg->wParam;
        return ht == HTMAXBUTTON || ht == HTMINBUTTON || ht == HTCLOSE;
    };

    switch (msg->message)
    {
        case WM_SHOWWINDOW:
            returnValue = this->handleSHOWWINDOW(msg);
            break;

        case WM_NCCALCSIZE:
            returnValue = this->handleNCCALCSIZE(msg, result);
            break;

        case WM_SIZE:
            returnValue = this->handleSIZE(msg);
            break;

        case WM_MOVE:
            returnValue = this->handleMOVE(msg);
            *result = 0;
            break;

        case WM_NCHITTEST:
            returnValue = this->handleNCHITTEST(msg, result);
            break;

        case WM_NCMOUSEHOVER:
        case WM_NCMOUSEMOVE: {
            if (!this->ui_.titlebarButtons)
            {
                break;
            }

            if (isHoveringTitlebarButton())
            {
                *result = 0;
                returnValue = true;

                POINT p{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
                ScreenToClient(msg->hwnd, &p);

                QPoint globalPos(p.x, p.y);
                globalPos /= this->devicePixelRatio();
                globalPos = this->mapToGlobal(globalPos);

                this->ui_.titlebarButtons->hover(msg->wParam, globalPos);
                this->lastEventWasNcMouseMove_ = true;
            }
            else
            {
                this->ui_.titlebarButtons->leave();
            }
        }
        break;

        case WM_MOUSEMOVE: {
            if (!this->lastEventWasNcMouseMove_)
            {
                break;
            }
            this->lastEventWasNcMouseMove_ = false;

            [[fallthrough]];
        }
        case WM_NCMOUSELEAVE: {
            if (this->ui_.titlebarButtons)
            {
                this->ui_.titlebarButtons->leave();
            }
        }
        break;

        case WM_DPICHANGED: {
            if (this->flags_.has(ClearBuffersOnDpiChange))
            {
                postToThread([] {
                    getApp()->getWindows()->invalidateChannelViewBuffers();
                });
            }
        }
        break;

        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONUP: {
            if (!this->ui_.titlebarButtons || !isHoveringTitlebarButton())
            {
                break;
            }
            returnValue = true;
            *result = 0;

            auto ht = msg->wParam;

            POINT p{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
            ScreenToClient(msg->hwnd, &p);

            QPoint globalPos(p.x, p.y);
            globalPos /= this->devicePixelRatio();
            globalPos = this->mapToGlobal(globalPos);

            if (msg->message == WM_NCLBUTTONDOWN)
            {
                this->ui_.titlebarButtons->mousePress(ht, globalPos);
            }
            else
            {
                this->ui_.titlebarButtons->mouseRelease(ht, globalPos);
            }
        }
        break;

        default:
            return QWidget::nativeEvent(eventType, message, result);
    }

    QWidget::nativeEvent(eventType, message, result);

    return returnValue;
#else
    return QWidget::nativeEvent(eventType, message, result);
#endif
}

void BaseWindow::scaleChangedEvent(float scale)
{
#ifdef USEWINSDK
    this->calcButtonsSizes();
#endif

    this->setFont(
        getApp()->getFonts()->getFont(FontStyle::UiTabs, this->scale()));
}

void BaseWindow::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    this->drawOutline(painter);
    this->drawCustomWindowFrame(painter);
}

void BaseWindow::drawOutline(QPainter &painter)
{
    if (this->frameless_)
    {
        painter.setPen(QColor("#999"));
        painter.drawRect(0, 0, this->width() - 1, this->height() - 1);
    }
}

float BaseWindow::desiredScale() const
{
    return getSettings()->getClampedUiScale();
}

void BaseWindow::updateScale()
{
    auto scale =
        this->flags_.has(DisableCustomScaling) ? 1 : this->desiredScale();

    this->setScale(scale);

    BaseWindow::applyScaleRecursive(this, scale);
}

void BaseWindow::applyScaleRecursive(QObject *root, float scale)
{
    for (QObject *obj : root->children())
    {
        auto *base = dynamic_cast<BaseWidget *>(obj);
        if (base)
        {
            auto *window = dynamic_cast<BaseWindow *>(obj);
            if (window)
            {
                continue;
            }
            base->setScale(scale);
        }

        applyScaleRecursive(obj, scale);
    }
}

#ifdef USEWINSDK
void BaseWindow::updateRealSize()
{
    auto hwnd = this->safeHWND();
    if (!hwnd)
    {
        return;
    }

    RECT real;
    ::GetWindowRect(*hwnd, &real);
    this->realBounds_ = QRect(real.left, real.top, real.right - real.left,
                              real.bottom - real.top);
}
#endif

void BaseWindow::calcButtonsSizes()
{
    if (!this->shown_)
    {
        return;
    }

    if (this->frameless_ || !this->ui_.titlebarButtons)
    {
        return;
    }

#ifdef USEWINSDK
    if ((static_cast<float>(this->width()) / this->scale()) < 300)
    {
        this->ui_.titlebarButtons->setSmallSize();
    }
    else
    {
        this->ui_.titlebarButtons->setRegularSize();
    }
#endif
}

void BaseWindow::drawCustomWindowFrame(QPainter &painter)
{
#ifdef USEWINSDK
    if (this->hasCustomWindowFrame())
    {
        QColor bg = this->overrideBackgroundColor_.value_or(
            this->theme->window.background);
        if (this->isMaximized_)
        {
            painter.fillRect(this->rect(), bg);
        }
        else
        {
            auto dpr = this->devicePixelRatio();
            if (dpr != 1)
            {
                painter.setTransform(QTransform::fromScale(1 / dpr, 1 / dpr));
            }

            if (isWindows11OrGreater())
            {
                painter.fillRect(0, 0, this->realBounds_.width() - 1,
                                 this->realBounds_.height() - 1, bg);
            }
            else
            {
                painter.fillRect(1, 1, this->realBounds_.width() - 2,
                                 this->realBounds_.height() - 2, bg);
            }
        }
    }
#endif
}

bool BaseWindow::handleSHOWWINDOW(MSG *msg)
{
#ifdef USEWINSDK

    if (!msg->wParam)
    {
        return true;
    }

    if (!this->shown_)
    {
        this->shown_ = true;

        if (this->hasCustomWindowFrame())
        {
            const MARGINS margins = {-1};
            DwmExtendFrameIntoClientArea(msg->hwnd, &margins);
        }

        if (!this->initalBounds_.isNull())
        {
            this->setGeometry(this->initalBounds_);
            this->currentBounds_ = this->initalBounds_;
        }

        this->calcButtonsSizes();
        this->updateRealSize();
    }

    return true;
#else
    return false;
#endif
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
bool BaseWindow::handleNCCALCSIZE(MSG *msg, qintptr *result)
#else
bool BaseWindow::handleNCCALCSIZE(MSG *msg, long *result)
#endif
{
#ifdef USEWINSDK
    if (!this->hasCustomWindowFrame())
    {
        return false;
    }

    if (msg->wParam != TRUE)
    {
        *result = 0;
        return true;
    }

    auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam);
    auto *r = &params->rgrc[0];

    WINDOWPLACEMENT wp;
    wp.length = sizeof(WINDOWPLACEMENT);
    this->isMaximized_ = GetWindowPlacement(msg->hwnd, &wp) != 0 &&
                         (wp.showCmd == SW_SHOWMAXIMIZED);

    auto borders = windowBordersFor(msg->hwnd, this->isMaximized_);
    r->left += borders.left;
    r->top += borders.top;
    r->right += borders.right;
    r->bottom += borders.bottom;

    if (borders.left != 0 || borders.top != 0 || borders.right != 0 ||
        borders.bottom != 0)
    {
        *result = 0;
        return true;
    }

    QPoint fixed = {r->left, r->top};
    params->rgrc[1] = {fixed.x(), fixed.y(), fixed.x() + 1, fixed.y() + 1};
    params->rgrc[2] = {fixed.x(), fixed.y(), fixed.x() + 1, fixed.y() + 1};
    *result = WVR_VALIDRECTS;

    return true;
#else
    return false;
#endif
}

bool BaseWindow::handleSIZE(MSG *msg)
{
#ifdef USEWINSDK
    if (this->ui_.windowLayout)
    {
        if (this->frameless_)
        {
        }
        else if (this->hasCustomWindowFrame())
        {
            this->isNotMinimizedOrMaximized_ = msg->wParam == SIZE_RESTORED;

            if (this->isNotMinimizedOrMaximized_)
            {
                postToThread([this] {
                    this->currentBounds_ = this->geometry();
                });
            }
            this->useNextBounds_.stop();

            if (msg->wParam == SIZE_MINIMIZED && this->ui_.titlebarButtons)
            {
                this->ui_.titlebarButtons->leave();
            }

            RECT real;
            ::GetWindowRect(msg->hwnd, &real);
            this->realBounds_ =
                QRect(real.left, real.top, real.right - real.left,
                      real.bottom - real.top);
        }
    }
    return false;
#else
    return false;
#endif
}

bool BaseWindow::handleMOVE(MSG *msg)
{
#ifdef USEWINSDK
    if (this->isNotMinimizedOrMaximized_)
    {
        this->useNextBounds_.start(10);
    }
#endif
    return false;
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
bool BaseWindow::handleNCHITTEST(MSG *msg, qintptr *result)
#else
bool BaseWindow::handleNCHITTEST(MSG *msg, long *result)
#endif
{
#ifdef USEWINSDK
    const LONG borderWidth = 8;

    auto rect = this->rect();

    POINT p{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
    ScreenToClient(msg->hwnd, &p);

    QPoint point(p.x, p.y);
    point /= this->devicePixelRatio();

    auto x = point.x();
    auto y = point.y();

    if (this->hasCustomWindowFrame())
    {
        *result = 0;

        bool resizeWidth =
            minimumWidth() != maximumWidth() && !this->isMaximized();
        bool resizeHeight =
            minimumHeight() != maximumHeight() && !this->isMaximized();

        if (resizeWidth)
        {
            if (x < rect.left() + borderWidth)
            {
                *result = HTLEFT;
            }

            if (x >= rect.right() - borderWidth)
            {
                *result = HTRIGHT;
            }
        }
        if (resizeHeight)
        {
            if (y >= rect.bottom() - borderWidth)
            {
                *result = HTBOTTOM;
            }

            if (y < rect.top() + borderWidth)
            {
                *result = HTTOP;
            }
        }
        if (resizeWidth && resizeHeight)
        {
            if (x >= rect.left() && x < rect.left() + borderWidth &&
                y < rect.bottom() && y >= rect.bottom() - borderWidth)
            {
                *result = HTBOTTOMLEFT;
            }

            if (x < rect.right() && x >= rect.right() - borderWidth &&
                y < rect.bottom() && y >= rect.bottom() - borderWidth)
            {
                *result = HTBOTTOMRIGHT;
            }

            if (x >= rect.left() && x < rect.left() + borderWidth &&
                y >= rect.top() && y < rect.top() + borderWidth)
            {
                *result = HTTOPLEFT;
            }

            if (x < rect.right() && x >= rect.right() - borderWidth &&
                y >= rect.top() && y < rect.top() + borderWidth)
            {
                *result = HTTOPRIGHT;
            }
        }

        if (*result == 0)
        {
            if (this->ui_.layoutBase->geometry().contains(point))
            {
                *result = HTCLIENT;
            }

            if (*result == 0 &&
                this->ui_.titlebarBox->geometry().contains(point))
            {
                for (const auto *widget : this->ui_.buttons)
                {
                    if (!widget->isVisible() ||
                        !widget->geometry().contains(point))
                    {
                        continue;
                    }

                    if (const auto *btn =
                            dynamic_cast<const TitleBarButton *>(widget))
                    {
                        switch (btn->getButtonStyle())
                        {
                            case TitleBarButtonStyle::Minimize: {
                                *result = HTMINBUTTON;
                                break;
                            }
                            case TitleBarButtonStyle::Unmaximize:
                            case TitleBarButtonStyle::Maximize: {
                                *result = HTMAXBUTTON;
                                break;
                            }
                            case TitleBarButtonStyle::Close: {
                                *result = HTCLOSE;
                                break;
                            }
                            default: {
                                *result = HTCLIENT;
                                break;
                            }
                        }
                        break;
                    }
                    *result = HTCLIENT;
                    break;
                }
            }

            if (*result == 0)
            {
                *result = HTCAPTION;
            }
        }

        return true;
    }

    if (this->flags_.has(FramelessDraggable))
    {
        *result = 0;
        bool client = false;

        if (auto *widget = this->childAt(point))
        {
            std::function<bool(QWidget *)> recursiveCheckMouseTracking;
            recursiveCheckMouseTracking = [&](QWidget *widget) {
                if (widget == nullptr || widget->isHidden())
                {
                    return false;
                }

                if (widget->hasMouseTracking())
                {
                    return true;
                }

                if (widget == this)
                {
                    return false;
                }

                return recursiveCheckMouseTracking(widget->parentWidget());
            };

            if (recursiveCheckMouseTracking(widget))
            {
                client = true;
            }
        }

        if (client)
        {
            *result = HTCLIENT;
        }
        else
        {
            *result = HTCAPTION;
        }

        return true;
    }

    return false;
#else
    return false;
#endif
}

#ifdef USEWINSDK
std::optional<HWND> BaseWindow::safeHWND() const
{
    if (!this->testAttribute(Qt::WA_WState_Created))
    {
        return std::nullopt;
    }
    return reinterpret_cast<HWND>(this->winId());
}
#endif

}  // namespace chatterino

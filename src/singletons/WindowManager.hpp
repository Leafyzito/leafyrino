// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/FlagsEnum.hpp"
#include "util/SignalListener.hpp"
#include "widgets/splits/SplitContainer.hpp"

#include <pajlada/settings/settinglistener.hpp>
#include <QJsonArray>
#include <QObject>
#include <QPoint>
#include <QTimer>

#include <memory>
#include <set>

namespace chatterino {

class Settings;
class Args;
class Paths;
class Window;
class ChannelView;
class IndirectChannel;
class Split;
struct SplitDescriptor;
class Channel;
using ChannelPtr = std::shared_ptr<Channel>;
struct Message;
using MessagePtr = std::shared_ptr<const Message>;
class WindowLayout;
class Theme;
class Fonts;
class TrayController;

enum class MessageElementFlag : int64_t;
using MessageElementFlags = FlagsEnum<MessageElementFlag>;
enum class WindowType;

enum class SettingsDialogPreference;
class FramelessEmbedWindow;

class WindowManager final : public QObject
{
    Q_OBJECT

    Theme &themes;
    const Args &appArgs;

public:
    static const QString WINDOW_LAYOUT_FILENAME;

    explicit WindowManager(const Args &appArgs_, const Paths &paths,
                           Settings &settings, Theme &themes_, Fonts &fonts);
    ~WindowManager();

    WindowManager(const WindowManager &) = delete;
    WindowManager(WindowManager &&) = delete;
    WindowManager &operator=(const WindowManager &) = delete;
    WindowManager &operator=(WindowManager &&) = delete;

    static void encodeTab(SplitContainer *tab, bool isSelected,
                          QJsonObject &obj);
    static void encodeChannel(IndirectChannel channel, QJsonObject &obj);
    static void encodeFilters(Split *split, QJsonArray &arr);
    static IndirectChannel decodeChannel(const SplitDescriptor &descriptor);

    void showSettingsDialog(
        QWidget *parent,
        SettingsDialogPreference preference = SettingsDialogPreference());

    void showAccountSelectPopup(QPoint point);

    void layoutChannelViews(Channel *channel = nullptr);

    void forceLayoutChannelViews();

    void invalidateChannelViewBuffers(Channel *channel = nullptr);

    void repaintVisibleChatWidgets(Channel *channel = nullptr);
    void repaintGifEmotes();

    Window &getMainWindow();

    Window *getLastSelectedWindow() const;

    Window &createWindow(WindowType type, bool show = true,
                         QWidget *parent = nullptr);

    Window &openInPopup(ChannelPtr channel);

    void select(Split *split);
    void select(SplitContainer *container);

    void scrollToMessage(const MessagePtr &message);
    void openChannelOrMessageFromTray(const QString &channelName,
                                      const QString &messageId);
    void showMainWindow();
    bool hideMainWindowToTray();
    void notifyTrayHighlight(const Channel *channel, const MessagePtr &message,
                             bool playSound);

    QRect emotePopupBounds() const;
    void setEmotePopupBounds(QRect bounds);

    void initialize();
    void save();
    void closeAll();

    int getGeneration() const;
    void incGeneration();

    MessageElementFlags getWordFlags();
    void updateWordTypeMask();

    void sendAlert();

    void queueSave();

    void toggleAllOverlayInertia();

    std::set<QString> getVisibleChannelNames() const;
    QJsonArray getOpenTabSnapshot() const;

    pajlada::Signals::NoArgSignal gifRepaintRequested;

    pajlada::Signals::Signal<Channel *> layoutRequested;

    pajlada::Signals::Signal<Channel *> invalidateBuffersRequested;

    pajlada::Signals::NoArgSignal wordFlagsChanged;

    pajlada::Signals::Signal<Split *> selectSplit;
    pajlada::Signals::Signal<SplitContainer *> selectSplitContainer;
    pajlada::Signals::Signal<const MessagePtr &> scrollToMessageSignal;

private:
    static void encodeNodeRecursively(SplitContainer::Node *node,
                                      QJsonObject &obj);

    WindowLayout loadWindowLayoutFromFile() const;

    void applyWindowLayout(const WindowLayout &layout);

    const QString windowLayoutFilePath;

    bool shuttingDown_ = false;

    QRect emotePopupBounds_;

    std::atomic<int> generation_{0};

    std::vector<Window *> windows_;
    std::vector<Window *> trayHiddenWindows_;

    std::unique_ptr<FramelessEmbedWindow> framelessEmbedWindow_;
#ifndef Q_OS_MACOS
    std::unique_ptr<TrayController> trayController_;
#endif
    Window *mainWindow_{};
    Window *selectedWindow_{};

    MessageElementFlags wordFlags_{};

    QTimer *saveTimer;

    pajlada::Signals::SignalHolder signalHolder;

    SignalListener updateWordTypeMaskListener;
    SignalListener forceLayoutChannelViewsListener;
    SignalListener layoutChannelViewsListener;
    SignalListener invalidateChannelViewBuffersListener;
    SignalListener repaintVisibleChatWidgetsListener;

    friend class Window;
};

}  // namespace chatterino

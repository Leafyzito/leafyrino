// SPDX-FileCopyrightText: 2016 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/ChatterinoSetting.hpp"
#include "singletons/Paths.hpp"
#include "util/RapidJsonSerializeQString.hpp"

#include <pajlada/settings/setting.hpp>
#include <QColor>
#include <QJsonObject>
#include <QPalette>
#include <QPixmap>
#include <QString>
#include <QTimer>
#include <QVariant>

#include <memory>
#include <optional>
#include <vector>

namespace chatterino {

class WindowManager;

struct ThemeDescriptor {
    QString key;

    QString path;

    QString name;

    bool custom{};
};

class Theme final
{
public:
    static const std::vector<ThemeDescriptor> builtInThemes;

    static const ThemeDescriptor fallbackTheme;

    static const int AUTO_RELOAD_INTERVAL_MS = 500;

    Theme(const Paths &paths);

    bool isLightTheme() const;
    bool isSystemTheme() const;

    struct TabColors {
        QColor text;
        struct {
            QColor regular;
            QColor hover;
            QColor unfocused;
        } backgrounds;
        struct {
            QColor regular;
            QColor hover;
            QColor unfocused;
        } line;
    };

    struct TextColors {
        QColor regular;
        QColor caret;
        QColor link;
        QColor system;
        QColor chatPlaceholder;
    };

    struct MessageBackgrounds {
        QColor regular;
        QColor alternate;
    };

    QColor accent{"#00aeef"};

    struct {
        QColor background;
        QColor text;
    } window;

    struct {
        TabColors regular;
        TabColors newMessage;
        TabColors highlighted;
        TabColors selected;
        QColor dividerLine;

        QColor liveIndicator;
        QColor rerunIndicator;
    } tabs;

    struct {
        TextColors textColors;
        MessageBackgrounds backgrounds;

        QColor disabled;
        QColor selection;

        QColor highlightAnimationStart;
        QColor highlightAnimationEnd;
    } messages;

    struct {
        TextColors textColors;
        MessageBackgrounds backgrounds;

        QColor disabled;
        QColor selection;
        QColor background;
    } overlayMessages;

    struct {
        QColor background;
        QColor thumb;
        QColor thumbSelected;
    } scrollbars;

    struct {
        QColor messageSeperator;
        QColor background;
        QColor dropPreview;
        QColor dropPreviewBorder;
        QColor dropTargetRect;
        QColor dropTargetRectBorder;
        QColor resizeHandle;
        QColor resizeHandleBackground;

        struct {
            QColor border;
            QColor focusedBorder;
            QColor background;
            QColor focusedBackground;
            QColor text;
            QColor focusedText;
        } header;

        struct {
            QColor background;
            QColor backgroundPulse;
            QColor text;

            QString styleSheet;
        } input;
    } splits;

    struct {
        QPixmap copy;
    } buttons;

    QPalette palette;

    void normalizeColor(QColor &color) const;
    void update();

    bool isAutoReloading() const;
    void setAutoReload(bool autoReload);

    std::vector<std::pair<QString, QVariant>> availableThemes() const;

    pajlada::Signals::NoArgSignal updated;

    QStringSetting themeName{"/appearance/theme/name", "Dark"};
    QStringSetting lightSystemThemeName{"/appearance/theme/lightSystem",
                                        "Light"};
    QStringSetting darkSystemThemeName{"/appearance/theme/darkSystem", "Dark"};

private:
    bool isLight_ = false;

    std::vector<ThemeDescriptor> availableThemes_;

    QString currentThemePath_;
    std::unique_ptr<QTimer> themeReloadTimer_;

    QJsonObject currentThemeJson_;

    QObject lifetime_;

    void loadAvailableThemes(const Paths &paths);

    std::optional<ThemeDescriptor> findThemeByKey(const QString &key);

    void parseFrom(const QJsonObject &root, bool isCustomTheme);

    pajlada::Signals::NoArgSignal repaintVisibleChatWidgets_;

    friend class WindowManager;
};

Theme *getTheme();
}

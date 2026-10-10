#pragma once

#include "singletons/Theme.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QObject>
#include <QPalette>
#include <QPointer>
#include <QWidget>

#include <functional>
#include <utility>

namespace chatterino {

inline QPalette moltorinoDialogPalette(const Theme &theme)
{
    auto palette = theme.palette;
    palette.setColor(QPalette::Window, theme.window.background);
    palette.setColor(QPalette::WindowText, theme.window.text);
    palette.setColor(QPalette::Base, theme.tabs.regular.backgrounds.regular);
    palette.setColor(QPalette::AlternateBase,
                     theme.tabs.regular.backgrounds.hover);
    palette.setColor(QPalette::Text, theme.window.text);
    palette.setColor(QPalette::Button, theme.tabs.selected.backgrounds.regular);
    palette.setColor(QPalette::ButtonText, theme.window.text);
    palette.setColor(QPalette::Highlight,
                     theme.tabs.selected.backgrounds.regular);
    palette.setColor(QPalette::HighlightedText, theme.tabs.selected.text);
    palette.setColor(QPalette::PlaceholderText,
                     theme.messages.textColors.chatPlaceholder);
    palette.setColor(QPalette::Link, theme.accent);
    palette.setColor(QPalette::LinkVisited, theme.accent);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    palette.setColor(QPalette::Accent, theme.accent);
#endif
    palette.setColor(QPalette::Mid, theme.tabs.dividerLine);
    palette.setColor(QPalette::Dark, theme.tabs.dividerLine);
    palette.setColor(QPalette::ToolTipBase,
                     theme.tabs.selected.backgrounds.regular);
    palette.setColor(QPalette::ToolTipText, theme.window.text);
    palette.setColor(QPalette::BrightText,
                     theme.isLightTheme() ? QColor(QStringLiteral("#a1262f"))
                                          : QColor(QStringLiteral("#ff7a82")));
    palette.setColor(QPalette::Disabled, QPalette::Text,
                     theme.tabs.regular.text);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText,
                     theme.tabs.regular.text);
    palette.setColor(QPalette::Disabled, QPalette::Button,
                     theme.tabs.regular.backgrounds.unfocused);
    return palette;
}

inline void applyMoltorinoDialogPalette(QWidget *root, const Theme &theme)
{
    const auto palette = moltorinoDialogPalette(theme);
    root->setAttribute(Qt::WA_WindowPropagation);
    root->setPalette(palette);
    for (auto *child : root->findChildren<QWidget *>())
    {
        child->setPalette(palette);
    }
}

inline QString moltorinoDialogStyleSheet()
{
    return QStringLiteral(R"(
        QLabel#SecondaryText {
            color: palette(placeholder-text);
        }
        QAbstractItemView {
            selection-background-color: palette(highlight);
            selection-color: palette(highlighted-text);
        }
    )");
}

namespace detail {

class MoltorinoDialogThemeBinding final : public QObject
{
public:
    MoltorinoDialogThemeBinding(QWidget *root, std::function<void()> afterApply)
        : QObject(root)
        , root_(root)
        , afterApply_(std::move(afterApply))
    {
        this->connections_.managedConnect(getTheme()->updated, [this] {
            this->apply();
        });
        this->apply();
    }

private:
    void apply()
    {
        if (!this->root_)
        {
            return;
        }

        applyMoltorinoDialogPalette(this->root_, *getTheme());
        if (this->afterApply_)
        {
            this->afterApply_();
        }
    }

    QPointer<QWidget> root_;
    std::function<void()> afterApply_;
    pajlada::Signals::SignalHolder connections_;
};

}  // namespace detail

inline void installMoltorinoDialogTheme(QWidget *root,
                                        std::function<void()> afterApply = {})
{
    root->setAttribute(Qt::WA_WindowPropagation);
    root->setAutoFillBackground(true);
    new detail::MoltorinoDialogThemeBinding(root, std::move(afterApply));
}

}  // namespace chatterino

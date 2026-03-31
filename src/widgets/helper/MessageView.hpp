// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/Link.hpp"
#include "messages/Message.hpp"
#include "widgets/BaseWidget.hpp"

#include <QPointer>
#include <QWidget>

class QEvent;
class QMouseEvent;
class QPaintEvent;

namespace chatterino {

class MessageLayout;
class TooltipWidget;
class LinkInfo;

/// MessageView is a fixed-width widget that displays a single message.
/// For the message to be rendered, you must call setWidth.
class MessageView : public BaseWidget
{
    Q_OBJECT

public:
    explicit MessageView(QWidget *parent = nullptr);
    ~MessageView() override;
    MessageView(const MessageView &) = delete;
    MessageView(MessageView &&) = delete;
    MessageView &operator=(const MessageView &) = delete;
    MessageView &operator=(MessageView &&) = delete;

    void setMessage(const MessagePtr &message);
    /// Renders @a message with the same element flags as the main chat view
    /// (timestamps, username styling, emotes, etc.).
    void setFullMessage(const MessagePtr &message);
    void clearMessage();

    void setWidth(int width);
    void relayout();

protected:
    void paintEvent(QPaintEvent *event) override;
    void themeChangedEvent() override;
    void scaleChangedEvent(float newScale) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void createMessageLayout();
    void layoutMessage();
    void updateHoverTooltip(QMouseEvent *event);
    void setLinkInfoTooltip(LinkInfo *info);
    void pendingLinkInfoStateChanged();
    void handleLinkClick(QMouseEvent *event, const Link &link);
    void showUserInfoPopup(const QString &userName);

    bool layoutUsesChatWordFlags_{false};
    bool hasAnimatedElements_{false};
    MessagePtr message_;
    std::unique_ptr<MessageLayout> messageLayout_;

    MessageColors messageColors_;
    MessagePreferences messagePreferences_;

    int width_{};

    TooltipWidget *tooltipWidget_{};
    QPointer<LinkInfo> pendingLinkInfo_;
};

}  // namespace chatterino

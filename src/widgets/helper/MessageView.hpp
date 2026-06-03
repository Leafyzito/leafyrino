// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/Message.hpp"
#include "widgets/BaseWidget.hpp"

#include <QWidget>

namespace chatterino {

class MessageLayout;

class MessageView : public BaseWidget
{
    Q_OBJECT

public:
    MessageView();
    ~MessageView() override;
    MessageView(const MessageView &) = delete;
    MessageView(MessageView &&) = delete;
    MessageView &operator=(const MessageView &) = delete;
    MessageView &operator=(MessageView &&) = delete;

    void setMessage(const MessagePtr &message);
    void clearMessage();

    void setWidth(int width);

protected:
    void paintEvent(QPaintEvent *event) override;
    void themeChangedEvent() override;
    void scaleChangedEvent(float newScale) override;

private:
    void createMessageLayout();
    void layoutMessage();

    MessagePtr message_;
    std::unique_ptr<MessageLayout> messageLayout_;

    MessageColors messageColors_;
    MessagePreferences messagePreferences_;

    int width_{};
};

}

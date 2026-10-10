// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWindow.hpp"

#include <QString>

#include <cstdint>
#include <memory>

namespace chatterino {

class Channel;
class ChatterListWidgetPrivate;
enum class MessagePlatform : std::uint8_t;

class ChatterListWidget : public BaseWindow
{
    Q_OBJECT

public:
    ChatterListWidget(std::shared_ptr<Channel> channel, QWidget *parent);
    ~ChatterListWidget() override;

    static bool supportsChannel(const Channel *channel);
    const QString &channelName() const;

    Q_SIGNAL void userClicked(QString userLogin, MessagePlatform platform,
                              QString channelName, QString userId);

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;

private:
    std::unique_ptr<ChatterListWidgetPrivate> d_;
};

}  // namespace chatterino

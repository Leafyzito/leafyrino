// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWidget.hpp"

class QWebEngineView;

namespace chatterino {

class StreamPlayerView : public BaseWidget
{
    Q_OBJECT

public:
    explicit StreamPlayerView(QWidget *parent = nullptr);

    void loadPlayerUrl(const QString &url);
    void clear();

private:
    QWebEngineView *view_ = nullptr;
};

}  // namespace chatterino

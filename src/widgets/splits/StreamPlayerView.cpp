// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/StreamPlayerView.hpp"

#include <QVBoxLayout>
#include <QWebEngineSettings>
#include <QWebEngineView>

namespace chatterino {

StreamPlayerView::StreamPlayerView(QWidget *parent)
    : BaseWidget(parent)
    , view_(new QWebEngineView(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(this->view_);

    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    this->view_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto *settings = this->view_->settings();
    settings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    settings->setAttribute(QWebEngineSettings::PluginsEnabled, true);
    settings->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture,
                           false);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls,
                           true);
    settings->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, true);
}

void StreamPlayerView::loadPlayerUrl(const QString &url)
{
    if (url.trimmed().isEmpty())
    {
        this->clear();
        return;
    }

    this->view_->load(QUrl(url));
}

void StreamPlayerView::clear()
{
    this->view_->setUrl(QUrl(QStringLiteral("about:blank")));
}

}  // namespace chatterino

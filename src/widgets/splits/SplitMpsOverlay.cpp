// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/SplitMpsOverlay.hpp"

#include "singletons/Settings.hpp"

#include <QPalette>

#include <algorithm>

namespace chatterino {

SplitMpsOverlay::SplitMpsOverlay(QWidget *parent)
    : BaseWidget(parent)
    , label_(new QLabel(this))
{
    this->setAttribute(Qt::WA_TransparentForMouseEvents);
    this->setAttribute(Qt::WA_NoSystemBackground);

    this->label_->setVisible(false);
    this->label_->setTextFormat(Qt::PlainText);
    this->label_->setAttribute(Qt::WA_TransparentForMouseEvents);
    this->label_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    {
        auto f = this->label_->font();
        f.setPointSizeF(std::max(8.0, f.pointSizeF() * 0.80));
        this->label_->setFont(f);
    }
    this->label_->raise();

    this->timer_.setParent(this);
    this->timer_.setInterval(250);
    QObject::connect(&this->timer_, &QTimer::timeout, this, [this] {
        this->updateOverlay();
    });

    // Settings wiring
    getSettings()->showSplitMps.connect(
        [this](const bool &enabled) {
            if (!enabled)
            {
                this->timer_.stop();
                this->timestamps_.clear();
                this->label_->setVisible(false);
                return;
            }
            this->updateOverlay();
            this->timer_.start();
        },
        this->signalHolder_);

    getSettings()->splitMpsCorner.connect(
        [this](const auto &) {
            this->updateGeometryForCorner();
        },
        this->signalHolder_);

    getSettings()->showSplitMpsWhenZero.connect(
        [this](const auto &) {
            this->updateOverlay();
        },
        this->signalHolder_);

    getSettings()->splitMpsWindow.connect(
        [this](const auto &) {
            this->updateOverlay();
        },
        this->signalHolder_);

    // Initial state
    this->updateOverlay();
    if (getSettings()->showSplitMps)
    {
        this->timer_.start();
    }
}

void SplitMpsOverlay::onMessageAdded()
{
    if (!getSettings()->showSplitMps)
    {
        return;
    }
    this->timestamps_.push_back(std::chrono::steady_clock::now());
}

void SplitMpsOverlay::setViewRect(const QRect &rect)
{
    this->viewRect_ = rect;
    this->updateGeometryForCorner();
}

void SplitMpsOverlay::themeChangedEvent()
{
    BaseWidget::themeChangedEvent();
    this->updateOverlay();
}

void SplitMpsOverlay::updateOverlay()
{
    if (!getSettings()->showSplitMps)
    {
        this->label_->setVisible(false);
        return;
    }

    const int windowSec =
        splitMpsWindowSeconds(getSettings()->splitMpsWindow.getEnum());

    const auto now = std::chrono::steady_clock::now();
    const auto cutoff = now - std::chrono::seconds(windowSec);
    while (!this->timestamps_.empty() && this->timestamps_.front() < cutoff)
    {
        this->timestamps_.pop_front();
    }

    const int count = static_cast<int>(this->timestamps_.size());
    const int mps = (count + windowSec / 2) / windowSec;
    const bool showWhenZero = getSettings()->showSplitMpsWhenZero;
    if (mps == 0 && !showWhenZero)
    {
        this->label_->setVisible(false);
        return;
    }

    // Faint text color derived from palette (works across themes)
    auto c = this->palette().color(QPalette::WindowText);
    c.setAlphaF(0.45);
    auto p = this->label_->palette();
    p.setColor(QPalette::WindowText, c);
    this->label_->setPalette(p);

    this->label_->setText(QString::number(mps) + " mps");
    this->label_->adjustSize();
    this->updateGeometryForCorner();
    this->label_->setVisible(true);
    this->label_->raise();
}

void SplitMpsOverlay::updateGeometryForCorner()
{
    if (!this->label_ || !this->label_->isVisible() || this->viewRect_.isNull())
    {
        return;
    }

    const int margin = 6;
    const QSize sz = this->label_->size();

    int x = this->viewRect_.left() + margin;
    int y = this->viewRect_.top() + margin;

    switch (getSettings()->splitMpsCorner.getEnum())
    {
        case SplitMpsCorner::TopLeft:
            break;
        case SplitMpsCorner::TopRight:
            x = this->viewRect_.right() - margin - sz.width() + 1;
            break;
        case SplitMpsCorner::BottomLeft:
            y = this->viewRect_.bottom() - margin - sz.height() + 1;
            break;
        case SplitMpsCorner::BottomRight:
            x = this->viewRect_.right() - margin - sz.width() + 1;
            y = this->viewRect_.bottom() - margin - sz.height() + 1;
            break;
    }

    this->label_->move(x, y);
    this->label_->raise();
}

}  // namespace chatterino

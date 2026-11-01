#include "widgets/helper/AutoModReviewBar.hpp"

#include "Application.hpp"
#include "controllers/automod/AutoModReviewController.hpp"
#include "messages/Message.hpp"
#include "widgets/helper/ChannelView.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QShowEvent>
#include <QSignalBlocker>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace chatterino {

AutoModReviewBar::AutoModReviewBar(QWidget *parent, ChannelView *view)
    : BaseWidget(parent)
    , view_(view)
    , channel_(new QComboBox(this))
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 1, 2, 1);
    layout->setSpacing(0);

    this->channel_->setMinimumContentsLength(8);
    this->channel_->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    this->channel_->setToolTip(u"Filter this AutoMod queue by channel"_s);
    layout->addWidget(this->channel_);

    QObject::connect(this->channel_, &QComboBox::currentIndexChanged, this,
                     [this] {
                         this->refresh();
                         this->applyFilter();
                     });
    if (auto *review = getApp()->getAutoModReview())
    {
        this->signalHolder_.managedConnect(review->countsChanged, [this] {
            if (this->active_ && this->isVisible())
            {
                this->refresh();
            }
        });
    }
    this->setActive(false);
    this->themeChangedEvent();
}

void AutoModReviewBar::setActive(bool active)
{
    if (this->active_ == active)
    {
        this->setVisible(active);
        return;
    }
    this->active_ = active;
    this->setVisible(active);
    if (!active)
    {
        this->view_->setMessagePredicate({});
        this->view_->setAutoModReviewSelectedKey({});
        return;
    }
    this->refresh();
    this->applyFilter();
    if (this->view_->isVisible() &&
        this->view_->autoModReviewSelectedKey().isEmpty())
    {
        this->view_->selectAutoModReview(1, true);
    }
}

QString AutoModReviewBar::selectedChannel() const
{
    return this->channel_->currentData().toString();
}

void AutoModReviewBar::setSelectedChannel(QString channel)
{
    channel = channel.trimmed().toLower();
    if (channel.startsWith(u'#'))
    {
        channel.remove(0, 1);
    }
    {
        const QSignalBlocker blocker(this->channel_);
        auto index = this->channel_->findData(channel);
        if (!channel.isEmpty() && index < 0)
        {
            this->channel_->addItem(u"#%1 (0)"_s.arg(channel), channel);
            index = this->channel_->count() - 1;
        }
        this->channel_->setCurrentIndex(channel.isEmpty() ? 0 : index);
    }
    if (this->active_)
    {
        this->refresh();
        this->applyFilter();
    }
}

void AutoModReviewBar::refresh()
{
    auto *review = getApp()->getAutoModReview();
    if (review == nullptr)
    {
        return;
    }

    const auto previousChannel = this->selectedChannel();
    const auto summary = review->activeSummary();
    {
        const QSignalBlocker blocker(this->channel_);
        this->channel_->clear();
        this->channel_->addItem(u"All (%1)"_s.arg(summary.total), QString{});
        if (!previousChannel.isEmpty())
        {
            this->channel_->addItem(u"#%1 (0)"_s.arg(previousChannel),
                                    previousChannel);
        }
        for (const auto &channel : summary.channels)
        {
            const auto label =
                u"#%1 (%2)"_s.arg(channel.login).arg(channel.count);
            const auto index = this->channel_->findData(channel.login);
            if (index < 0)
            {
                this->channel_->addItem(label, channel.login);
            }
            else
            {
                this->channel_->setItemText(index, label);
            }
        }
        const auto previousIndex = this->channel_->findData(previousChannel);
        this->channel_->setCurrentIndex(previousIndex >= 0 ? previousIndex : 0);
    }
    this->channel_->setToolTip(
        u"Filter this AutoMod queue by channel. Current: %1"_s.arg(
            this->channel_->currentText()));
}

void AutoModReviewBar::applyFilter()
{
    if (!this->active_)
    {
        return;
    }
    auto *review = getApp()->getAutoModReview();
    if (review == nullptr)
    {
        return;
    }
    const auto channel = this->selectedChannel();
    this->view_->setMessagePredicate([review,
                                      channel](const MessagePtr &message) {
        if (message && message->flags.has(MessageFlag::System) &&
            !message->flags.has(MessageFlag::AutoMod) &&
            message->channelName.isEmpty())
        {
            return true;
        }
        if (!message || !message->autoModReview)
        {
            return channel.isEmpty();
        }
        const auto *item = review->find(message->autoModReview->key);
        if (item == nullptr)
        {
            return false;
        }
        if (!channel.isEmpty() &&
            item->broadcasterLogin.compare(channel, Qt::CaseInsensitive) != 0)
        {
            return false;
        }
        return automod::reviewGroup(*item) != automod::ReviewGroup::Resolved;
    });

    const auto selected = this->view_->autoModReviewSelectedKey();
    const auto *selectedItem = review->find(selected);
    const bool selectedVisible =
        selectedItem != nullptr &&
        (channel.isEmpty() || selectedItem->broadcasterLogin.compare(
                                  channel, Qt::CaseInsensitive) == 0) &&
        automod::reviewGroup(*selectedItem) != automod::ReviewGroup::Resolved;
    if (!selectedVisible)
    {
        this->view_->setAutoModReviewSelectedKey({});
        if (this->view_->isVisible())
        {
            this->view_->selectAutoModReview(1, true);
        }
    }
    if (this->view_->isVisible())
    {
        this->view_->setFocus(Qt::OtherFocusReason);
    }
}

void AutoModReviewBar::themeChangedEvent()
{
    this->setStyleSheet(
        u"AutoModReviewBar { background: transparent; border: none; }"_s);
    if (this->active_ && this->isVisible())
    {
        this->refresh();
    }
}

void AutoModReviewBar::scaleChangedEvent(float scale)
{
    const auto controlHeight = std::max(20, static_cast<int>(22 * scale));
    this->channel_->setFixedHeight(controlHeight);
    this->channel_->setFixedWidth(static_cast<int>(104 * scale));
    this->setFixedHeight(std::max(24, static_cast<int>(26 * scale)));
}

void AutoModReviewBar::showEvent(QShowEvent *event)
{
    BaseWidget::showEvent(event);
    if (this->active_)
    {
        this->refresh();
    }
}

}

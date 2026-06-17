// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/SearchPopup.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/filters/FilterSet.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "messages/MessageElement.hpp"
#include "messages/search/AuthorPredicate.hpp"
#include "messages/search/BadgePredicate.hpp"
#include "messages/search/ChannelPredicate.hpp"
#include "messages/search/LinkPredicate.hpp"
#include "messages/search/MessageFlagsPredicate.hpp"
#include "messages/search/RegexPredicate.hpp"
#include "messages/search/SubstringPredicate.hpp"
#include "messages/search/SubtierPredicate.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/splits/Split.hpp"

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPushButton>

namespace chatterino {

ChannelPtr SearchPopup::filter(const QString &text, const QString &channelName,
                               const std::vector<MessagePtr> &snapshot)
{
    ChannelPtr channel(new Channel(channelName, Channel::Type::None));

    auto predicates = parsePredicates(text);

    for (size_t i = 0; i < snapshot.size(); ++i)
    {
        MessagePtr message = snapshot[i];

        bool accept = true;
        for (const auto &pred : predicates)
        {
            if (!pred->appliesTo(*message))
            {
                accept = false;
                break;
            }
        }

        if (accept)
        {
            auto overrideFlags = std::optional<MessageFlags>(message->flags);
            overrideFlags->set(MessageFlag::DoNotLog);

            channel->addMessage(message, MessageContext::Repost, overrideFlags);
        }
    }

    return channel;
}

SearchPopup::SearchPopup(QWidget *parent, Split *split)
    : BasePopup(
          {
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
          },
          parent)
    , split_(split)
{
    this->initLayout();
    if (this->split_ && this->split_->getChannelView().hasSelection())
    {
        this->searchInput_->setText(
            this->split_->getChannelView().getSelectedText().trimmed());
        this->searchInput_->selectAll();
    }
    this->resize(400, 600);
    this->addShortcuts();

    this->themeChangedEvent();
}

void SearchPopup::addShortcuts()
{
    HotkeyController::HotkeyMap actions{
        {"search",
         [this](const std::vector<QString> &) -> QString {
             this->searchInput_->setFocus();
             this->searchInput_->selectAll();
             return "";
         }},
        {"delete",
         [this](const std::vector<QString> &) -> QString {
             this->close();
             return "";
         }},

        {"reject", nullptr},
        {"accept", nullptr},
        {"openTab", nullptr},
        {"scrollPage", nullptr},
    };

    this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
        HotkeyCategory::PopupWindow, actions, this);
}

void SearchPopup::addChannel(ChannelView &channel)
{
    if (this->searchChannels_.empty())
    {
        this->channelView_->setSourceChannel(channel.underlyingChannel());
        this->channelName_ = channel.underlyingChannel()->getName();
    }
    else if (this->searchChannels_.size() == 1)
    {
        this->channelView_->setSourceChannel(
            std::make_shared<Channel>("multichannel", Channel::Type::None));

        auto flags = this->channelView_->getFlags();
        flags.set(MessageElementFlag::ChannelName);
        flags.unset(MessageElementFlag::ModeratorTools);
        this->channelView_->setOverrideFlags(flags);
    }

    this->searchChannels_.append(std::ref(channel));

    this->updateWindowTitle();
}

void SearchPopup::goToMessage(const MessagePtr &message)
{
    for (const auto &view : this->searchChannels_)
    {
        const auto type = view.get().underlyingChannel()->getType();
        if (type == Channel::Type::TwitchMentions ||
            type == Channel::Type::TwitchAutomod)
        {
            getApp()->getWindows()->scrollToMessage(message);
            return;
        }

        if (view.get().scrollToMessage(message))
        {
            return;
        }
    }
}

void SearchPopup::goToMessageId(const QString &messageId)
{
    for (const auto &view : this->searchChannels_)
    {
        if (view.get().scrollToMessageId(messageId))
        {
            return;
        }
    }
}

void SearchPopup::updateWindowTitle()
{
    QString historyName;

    if (this->searchChannels_.size() > 1)
    {
        this->setWindowTitle("Searching all open tabs");
        this->searchInput_->setPlaceholderText("Search all open tabs");
        return;
    }
    else if (this->channelName_ == "/automod")
    {
        historyName = "automod";
    }
    else if (this->channelName_ == "/mentions")
    {
        historyName = "mentions";
    }
    else if (this->channelName_ == "/whispers")
    {
        historyName = "whispers";
    }
    else if (this->channelName_.isEmpty())
    {
        historyName = "<empty>'s";
    }
    else
    {
        historyName = QString("%1's").arg(this->channelName_);
    }
    this->setWindowTitle("Searching in " + historyName + " history");
    this->searchInput_->setPlaceholderText("Type to search");
}

void SearchPopup::showEvent(QShowEvent *e)
{
    this->search();
    BaseWindow::showEvent(e);
}

bool SearchPopup::eventFilter(QObject *object, QEvent *event)
{
    if (object == this->searchInput_)
    {
        if (event->type() == QEvent::Resize)
        {
            this->layoutResultCountLabel();
        }
        else if (event->type() == QEvent::KeyPress)
        {
            QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent == QKeySequence::DeleteStartOfWord &&
                this->searchInput_->selectionLength() > 0)
            {
                this->searchInput_->backspace();
                return true;
            }
        }
    }
    return false;
}

void SearchPopup::themeChangedEvent()
{
    BasePopup::themeChangedEvent();

    this->setPalette(getTheme()->palette);
    this->updateResultCountLabelStyle();
}

void SearchPopup::updateResultCountLabelStyle()
{
    if (this->resultCountLabel_ == nullptr)
    {
        return;
    }

    const auto color =
        this->searchInput_ != nullptr
            ? this->searchInput_->palette().color(QPalette::PlaceholderText)
            : getTheme()->palette.color(QPalette::PlaceholderText);
    this->resultCountLabel_->setStyleSheet(
        QStringLiteral("color: %1; background: transparent;")
            .arg(color.name(QColor::HexRgb)));
}

void SearchPopup::search()
{
    if (this->snapshot_.size() == 0)
    {
        this->snapshot_ = this->buildSnapshot();
    }

    const auto total = this->snapshot_.size();
    auto channel =
        filter(this->searchInput_->text(), this->channelName_, this->snapshot_);
    this->channelView_->setChannel(channel);
    this->updateResultCount(channel->countMessages(), total);
}

void SearchPopup::updateResultCount(size_t matches, size_t total)
{
    if (this->searchInput_->text().trimmed().isEmpty())
    {
        this->resultCountLabel_->setText(QString::number(total));
        this->resultCountLabel_->setToolTip(
            QString("%1 messages in history").arg(total));
    }
    else
    {
        this->resultCountLabel_->setText(
            QString("%1/%2").arg(matches).arg(total));
        this->resultCountLabel_->setToolTip(
            QString("%1 matching messages out of %2 total")
                .arg(matches)
                .arg(total));
    }

    const QFontMetrics fm(this->resultCountLabel_->font());
    const int counterWidth =
        fm.horizontalAdvance(this->resultCountLabel_->text());
    constexpr int CLEAR_BUTTON_PADDING = 28;
    this->searchInput_->setTextMargins(
        0, 0, counterWidth + CLEAR_BUTTON_PADDING + 4, 0);
    this->layoutResultCountLabel();
}

void SearchPopup::layoutResultCountLabel()
{
    if (this->resultCountLabel_ == nullptr || this->searchInput_ == nullptr)
    {
        return;
    }

    const QFontMetrics fm(this->resultCountLabel_->font());
    const int labelWidth =
        fm.horizontalAdvance(this->resultCountLabel_->text());
    const int labelHeight = fm.height();

    constexpr int CLEAR_BUTTON_PADDING = 28;
    const int x =
        this->searchInput_->width() - CLEAR_BUTTON_PADDING - labelWidth;
    const int y = (this->searchInput_->height() - labelHeight) / 2;

    this->resultCountLabel_->setGeometry(x, y, labelWidth, labelHeight);
}

std::vector<MessagePtr> SearchPopup::buildSnapshot()
{
    if (this->searchChannels_.length() == 1)
    {
        const auto channelPtr = this->searchChannels_.at(0);
        return channelPtr.get().channel()->getMessageSnapshot();
    }

    auto combinedSnapshot = std::vector<std::shared_ptr<const Message>>{};
    for (auto &channel : this->searchChannels_)
    {
        ChannelView &sharedView = channel.get();

        const FilterSetPtr filterSet = sharedView.getFilterSet();
        std::vector<MessagePtr> snapshot =
            sharedView.channel()->getMessageSnapshot();

        for (const auto &message : snapshot)
        {
            if (filterSet &&
                !filterSet->filter(message, sharedView.underlyingChannel()))
            {
                continue;
            }

            combinedSnapshot.push_back(message);
        }
    }

    std::sort(combinedSnapshot.begin(), combinedSnapshot.end(),
              [](MessagePtr &a, MessagePtr &b) {
                  return a->id > b->id;
              });

    auto uniqueIterator =
        std::unique(combinedSnapshot.begin(), combinedSnapshot.end(),
                    [](MessagePtr &a, MessagePtr &b) {
                        return !a->id.isEmpty() && a->id == b->id;
                    });

    combinedSnapshot.erase(uniqueIterator, combinedSnapshot.end());

    std::sort(combinedSnapshot.begin(), combinedSnapshot.end(),
              [](MessagePtr &a, MessagePtr &b) {
                  return a->serverReceivedTime < b->serverReceivedTime;
              });

    return combinedSnapshot;
}

void SearchPopup::initLayout()
{
    {
        auto *layout1 = new QVBoxLayout(this);
        layout1->setContentsMargins(0, 0, 0, 0);
        layout1->setSpacing(0);

        {
            auto *layout2 = new QHBoxLayout();
            layout2->setContentsMargins(8, 8, 8, 8);
            layout2->setSpacing(8);

            {
                this->searchInput_ = new QLineEdit(this);
                layout2->addWidget(this->searchInput_);

                this->resultCountLabel_ = new QLabel(this->searchInput_);
                this->resultCountLabel_->setAttribute(
                    Qt::WA_TransparentForMouseEvents);
                this->resultCountLabel_->raise();
                this->updateResultCountLabelStyle();

                this->searchInput_->setPlaceholderText("Type to search");
                this->searchInput_->setClearButtonEnabled(true);
                this->searchInput_->findChild<QAbstractButton *>()->setIcon(
                    QPixmap(":/buttons/clearSearch.png"));
                QObject::connect(this->searchInput_, &QLineEdit::textChanged,
                                 this, &SearchPopup::search);
                this->searchInput_->installEventFilter(this);
            }

            layout1->addLayout(layout2);
        }

        {
            this->channelView_ = new ChannelView(
                this, this->split_, ChannelView::Context::Search,
                getSettings()->scrollbackSplitLimit);

            layout1->addWidget(this->channelView_, 1);
        }

        this->setLayout(layout1);
    }

    this->searchInput_->setFocus();
}

std::vector<std::unique_ptr<MessagePredicate>> SearchPopup::parsePredicates(
    const QString &input)
{
    static QRegularExpression predicateRegex(
        R"lit((?<negation>[!\-])?(?:(?<name>\w+):(?<value>".+?"|[^\s]+))|[^\s]+?(?=$|\s))lit");
    static QRegularExpression trimQuotationMarksRegex(R"(^"|"$)");

    QRegularExpressionMatchIterator it = predicateRegex.globalMatch(input);

    std::vector<std::unique_ptr<MessagePredicate>> predicates;

    while (it.hasNext())
    {
        QRegularExpressionMatch match = it.next();

        QString name = match.captured("name");
        bool isNegated = !match.captured("negation").isEmpty();
        QString value = match.captured("value");
        value.remove(trimQuotationMarksRegex);

        if (name == "from")
        {
            predicates.push_back(
                std::make_unique<AuthorPredicate>(value, isNegated));
        }
        else if (name == "badge")
        {
            predicates.push_back(
                std::make_unique<BadgePredicate>(value, isNegated));
        }
        else if (name == "subtier")
        {
            predicates.push_back(
                std::make_unique<SubtierPredicate>(value, isNegated));
        }
        else if (name == "has" && value == "link")
        {
            predicates.push_back(std::make_unique<LinkPredicate>(isNegated));
        }
        else if (name == "in")
        {
            predicates.push_back(
                std::make_unique<ChannelPredicate>(value, isNegated));
        }
        else if (name == "is")
        {
            predicates.push_back(
                std::make_unique<MessageFlagsPredicate>(value, isNegated));
        }
        else if (name == "regex")
        {
            predicates.push_back(
                std::make_unique<RegexPredicate>(value, isNegated));
        }
        else
        {
            predicates.push_back(
                std::make_unique<SubstringPredicate>(match.captured()));
        }
    }

    return predicates;
}

}  // namespace chatterino

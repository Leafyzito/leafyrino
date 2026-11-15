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
#include "messages/search/ClientPredicate.hpp"
#include "messages/search/LinkPredicate.hpp"
#include "messages/search/MessageFlagsPredicate.hpp"
#include "messages/search/RegexPredicate.hpp"
#include "messages/search/SubstringPredicate.hpp"
#include "messages/search/SubtierPredicate.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/ChannelHelpers.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/splits/Split.hpp"

#include <QCheckBox>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

namespace chatterino {

namespace {

const QRegularExpression &predicateRegex()
{
    static const QRegularExpression regex(
        R"lit((?<negation>[!\-])?(?:(?<name>\w+):(?<value>".+?"|[^\s]+))|[^\s]+?(?=$|\s))lit");
    return regex;
}

const QRegularExpression &trimQuotationMarksRegex()
{
    static const QRegularExpression regex(R"(^"|"$)");
    return regex;
}

const QStringList FILTER_NAMES{
    "from", "badge", "subtier", "client", "has", "in", "is", "regex",
};

struct FilterOption {
    QString label;
    QString value;
};

const std::vector<FilterOption> MESSAGE_TYPE_OPTIONS{
    {"Deleted messages", "deleted"},
    {"Timeouts and bans", "timeout"},
    {"Replies", "reply"},
    {"Highlights", "highlighted"},
    {"First-time chatters", "first-msg"},
    {"Cheers", "cheer-msg"},
    {"Subscriptions", "sub"},
    {"Channel point redemptions", "redemption"},
    {"Announcements", "announcement"},
    {"Repeated messages", "repeated"},
    {"Shared chat", "shared"},
    {"Restricted users", "restricted"},
    {"Monitored users", "monitored"},
    {"System messages", "system"},
};

const std::vector<FilterOption> BADGE_OPTIONS{
    {"Broadcaster", "broadcaster"},
    {"Moderator", "moderator"},
    {"VIP", "vip"},
    {"Subscriber", "subscriber"},
    {"Founder", "founder"},
    {"Verified", "partner"},
    {"Lead moderator", "lead_moderator"},
    {"Gifted subs", "sub-gifter"},
    {"Top gifter", "sub-gift-leader"},
    {"Cheered bits", "bits"},
    {"Top cheerer", "bits-leader"},
    {"Prime", "premium"},
    {"Turbo", "turbo"},
    {"Artist", "artist-badge"},
    {"Predicted", "predictions"},
    {"Twitch staff", "staff"},
};

const std::vector<FilterOption> SUB_TIER_OPTIONS{
    {"Tier 1", "1"},
    {"Tier 2", "2"},
    {"Tier 3", "3"},
};

const std::vector<FilterOption> CLIENT_OPTIONS{
    {"Web", "web"},       {"Android", "android"},   {"iOS", "ios"},
    {"Mobile", "mobile"}, {"Abnormal", "abnormal"}, {"Unknown", "unknown"},
};

/// Finds the first non-negated `name:value` filter in the input
std::optional<QRegularExpressionMatch> findFilter(const QString &input,
                                                  const QString &name)
{
    auto it = predicateRegex().globalMatch(input);
    while (it.hasNext())
    {
        auto match = it.next();
        if (match.captured("name") == name &&
            match.captured("negation").isEmpty())
        {
            return match;
        }
    }
    return std::nullopt;
}

QStringList filterValues(const QRegularExpressionMatch &match)
{
    QString value = match.captured("value");
    value.remove(trimQuotationMarksRegex());
    return value.split(',', Qt::SkipEmptyParts);
}

QString formatFilter(const QString &name, const QStringList &values)
{
    auto joined = values.join(',');
    if (joined.contains(' '))
    {
        joined = '"' + joined + '"';
    }
    return name + ':' + joined;
}

/// Removes a token from the text along with one adjacent space
void removeToken(QString &text, qsizetype start, qsizetype length)
{
    if (start > 0 && text.at(start - 1) == ' ')
    {
        start--;
        length++;
    }
    else if (start + length < text.size() && text.at(start + length) == ' ')
    {
        length++;
    }
    text.remove(start, length);
}

}  // namespace

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
    this->filterButton_->setChecked(getSettings()->searchFiltersPanelOpen);
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
    this->updateChannelFilters();
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

    // remove any duplicate messages from splits containing the same channel
    deduplicateMessages(combinedSnapshot);

    std::sort(combinedSnapshot.begin(), combinedSnapshot.end(),
              [](MessagePtr &a, MessagePtr &b) {
                  return a->timestamp < b->timestamp;
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

            {
                this->filterButton_ = new QPushButton("Filters", this);
                this->filterButton_->setCheckable(true);
                this->filterButton_->setToolTip(
                    "Narrow down the results by message type, user, and more");
                QObject::connect(this->filterButton_, &QPushButton::toggled,
                                 this, &SearchPopup::setFilterPanelOpen);
                layout2->addWidget(this->filterButton_);
            }

            layout1->addLayout(layout2);
        }

        {
            auto *layout2 = new QHBoxLayout();
            layout2->setContentsMargins(0, 0, 0, 0);
            layout2->setSpacing(0);

            this->channelView_ = new ChannelView(
                this, this->split_, ChannelView::Context::Search,
                getSettings()->scrollbackSplitLimit);
            layout2->addWidget(this->channelView_, 1);

            this->filterPanel_ = this->createFilterPanel();
            this->filterPanel_->hide();
            layout2->addWidget(this->filterPanel_);

            layout1->addLayout(layout2, 1);
        }

        this->setLayout(layout1);
    }

    this->searchInput_->setFocus();
}

QScrollArea *SearchPopup::createFilterPanel()
{
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(8, 0, 8, 8);
    layout->setSpacing(4);

    const auto addCheckBox = [this](QVBoxLayout *layout, const QString &label,
                                    const QString &name, const QString &value) {
        auto *checkBox = new QCheckBox(label);
        QObject::connect(checkBox, &QCheckBox::clicked, this,
                         [this, name, value](bool checked) {
                             this->setFilterValue(name, value, checked);
                         });
        layout->addWidget(checkBox);
        this->filterCheckBoxes_.push_back({checkBox, name, value});
    };

    const auto addSection = [this, layout](const QString &title,
                                           bool expanded) {
        auto *header = new QToolButton;
        header->setText(title);
        header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        header->setAutoRaise(true);
        header->setCheckable(true);
        header->setChecked(expanded);
        header->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        auto font = header->font();
        font.setBold(true);
        header->setFont(font);

        auto *body = new QWidget;
        auto *bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(12, 0, 0, 4);
        bodyLayout->setSpacing(2);
        body->setVisible(expanded);

        QObject::connect(
            header, &QToolButton::toggled, body, [header, body](bool open) {
                header->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
                body->setVisible(open);
            });

        layout->addWidget(header);
        layout->addWidget(body);
        this->filterSections_.push_back({header, body});
        return bodyLayout;
    };

    this->clearFiltersButton_ = new QPushButton("Clear all");
    QObject::connect(this->clearFiltersButton_, &QPushButton::clicked, this,
                     &SearchPopup::clearFilters);
    layout->addWidget(this->clearFiltersButton_);

    addCheckBox(layout, "Only messages with links", "has", "link");

    // only the most common options are shown until expanded
    const auto addOptionsWithMore =
        [this, addCheckBox](QVBoxLayout *section, const QString &name,
                            const std::vector<FilterOption> &options,
                            size_t commonCount) {
            auto *more = new QWidget;
            auto *moreLayout = new QVBoxLayout(more);
            moreLayout->setContentsMargins(0, 0, 0, 0);
            moreLayout->setSpacing(2);
            more->hide();

            for (size_t i = 0; i < options.size(); i++)
            {
                addCheckBox(i < commonCount ? section : moreLayout,
                            options[i].label, name, options[i].value);
            }
            section->addWidget(more);

            auto *button = new QToolButton;
            button->setText("Show more");
            button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            button->setArrowType(Qt::DownArrow);
            button->setAutoRaise(true);
            button->setCheckable(true);
            QObject::connect(
                button, &QToolButton::toggled, more, [button, more](bool show) {
                    more->setVisible(show);
                    button->setArrowType(show ? Qt::UpArrow : Qt::DownArrow);
                    button->setText(show ? "Show less" : "Show more");
                });
            section->addWidget(button, 0, Qt::AlignLeft);
            this->filterMoreGroups_.push_back({button, more});
        };

    addOptionsWithMore(addSection("Message type", true), "is",
                       MESSAGE_TYPE_OPTIONS, 6);

    {
        auto *section = addSection("From user", true);
        this->fromUsersInput_ = new QLineEdit;
        this->fromUsersInput_->setPlaceholderText("username, username");
        this->fromUsersInput_->setClearButtonEnabled(true);
        QObject::connect(
            this->fromUsersInput_, &QLineEdit::textEdited, this,
            [this](const QString &text) {
                static const QRegularExpression separator(R"([,\s]+)");
                QStringList users;
                for (auto user : text.split(separator, Qt::SkipEmptyParts))
                {
                    if (user.startsWith('@'))
                    {
                        user.remove(0, 1);
                    }
                    if (!user.isEmpty())
                    {
                        users.append(user);
                    }
                }
                this->setFilterValues("from", users);
            });
        QObject::connect(this->fromUsersInput_, &QLineEdit::editingFinished,
                         this, &SearchPopup::syncFilterPanel);
        section->addWidget(this->fromUsersInput_);
    }

    addOptionsWithMore(addSection("Badges", true), "badge", BADGE_OPTIONS, 6);

    {
        auto *section = addSection("Sub tier", false);
        for (const auto &option : SUB_TIER_OPTIONS)
        {
            addCheckBox(section, option.label, "subtier", option.value);
        }
    }

    {
        auto *section = addSection("Sent from", false);
        for (const auto &option : CLIENT_OPTIONS)
        {
            addCheckBox(section, option.label, "client", option.value);
        }
    }

    {
        // can list every open channel, so it starts collapsed
        this->channelFiltersLayout_ = addSection("Channel", false);
        this->channelSection_ = this->filterSections_.back().body;
        this->filterSections_.back().header->hide();
    }

    layout->addStretch(1);

    auto *scrollArea = new QScrollArea;
    scrollArea->setWidget(content);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->viewport()->setAutoFillBackground(false);
    content->setAutoFillBackground(false);
    QObject::connect(this->searchInput_, &QLineEdit::textChanged, this,
                     &SearchPopup::syncFilterPanel);

    return scrollArea;
}

void SearchPopup::setFilterPanelOpen(bool open)
{
    if (this->filterPanel_->isVisibleTo(this) == open)
    {
        return;
    }

    if (open)
    {
        // hidden filters count too, so expanding a section doesn't clip it
        this->filterPanel_->widget()->ensurePolished();
        int contentWidth = this->fromUsersInput_->sizeHint().width();
        for (const auto &filter : this->filterCheckBoxes_)
        {
            if (filter.name != "in")
            {
                contentWidth =
                    std::max(contentWidth, filter.checkBox->sizeHint().width());
            }
        }
        // section indent and panel margins
        contentWidth += 12 + 16;
        this->filterPanel_->setFixedWidth(
            contentWidth +
            this->filterPanel_->verticalScrollBar()->sizeHint().width());
    }

    // grow the window instead of squeezing the messages
    const int panelWidth = this->filterPanel_->width();
    this->filterPanel_->setVisible(open);
    if (!this->isMaximized() && !this->isFullScreen())
    {
        this->resize(this->width() + (open ? panelWidth : -panelWidth),
                     this->height());
    }

    getSettings()->searchFiltersPanelOpen = open;
}

void SearchPopup::updateChannelFilters()
{
    std::erase_if(this->filterCheckBoxes_, [](const FilterCheckBox &filter) {
        if (filter.name == "in")
        {
            filter.checkBox->deleteLater();
            return true;
        }
        return false;
    });

    QStringList names;
    for (const auto &view : this->searchChannels_)
    {
        // skip empty splits and special channels like /mentions
        const auto name = view.get().channel()->getName();
        if (!name.isEmpty() && !name.startsWith('/'))
        {
            names.append(name);
        }
    }
    names.removeDuplicates();

    const bool show = names.size() > 1;
    for (const auto &section : this->filterSections_)
    {
        if (section.body == this->channelSection_)
        {
            section.header->setVisible(show);
            if (!show)
            {
                section.header->setChecked(false);
            }
        }
    }

    if (show)
    {
        for (const auto &name : names)
        {
            auto *checkBox = new QCheckBox(name);
            checkBox->setToolTip(name);
            // long names get cut off instead of widening the panel
            checkBox->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            QObject::connect(checkBox, &QCheckBox::clicked, this,
                             [this, name](bool checked) {
                                 this->setFilterValue("in", name, checked);
                             });
            this->channelFiltersLayout_->addWidget(checkBox);
            this->filterCheckBoxes_.push_back({checkBox, "in", name});
        }
    }

    this->syncFilterPanel();
}

void SearchPopup::syncFilterPanel()
{
    if (this->filterPanel_ == nullptr)
    {
        return;
    }

    int filterCount = 0;
    auto it = predicateRegex().globalMatch(this->searchInput_->text());
    while (it.hasNext())
    {
        auto match = it.next();
        if (FILTER_NAMES.contains(match.captured("name")))
        {
            filterCount += std::max<qsizetype>(filterValues(match).size(), 1);
        }
    }
    this->filterButton_->setText(filterCount > 0
                                     ? QString("Filters (%1)").arg(filterCount)
                                     : QString("Filters"));
    this->clearFiltersButton_->setEnabled(filterCount > 0);

    for (const auto &filter : this->filterCheckBoxes_)
    {
        const bool checked = this->activeFilterValues(filter.name)
                                 .contains(filter.value, Qt::CaseInsensitive);
        if (filter.checkBox->isChecked() == checked)
        {
            continue;
        }
        filter.checkBox->setChecked(checked);

        // reveal filters that were typed into the search box
        if (checked)
        {
            for (const auto &group : this->filterMoreGroups_)
            {
                if (group.body->isAncestorOf(filter.checkBox))
                {
                    group.header->setChecked(true);
                }
            }
            for (const auto &section : this->filterSections_)
            {
                if (section.body->isAncestorOf(filter.checkBox))
                {
                    section.header->setChecked(true);
                }
            }
        }
    }

    // don't fight the user while they're typing in the field
    const auto users = this->activeFilterValues("from");
    if (!this->fromUsersInput_->hasFocus())
    {
        this->fromUsersInput_->setText(users.join(", "));
    }
}

QStringList SearchPopup::activeFilterValues(const QString &name) const
{
    auto match = findFilter(this->searchInput_->text(), name);
    if (!match)
    {
        return {};
    }
    return filterValues(*match);
}

void SearchPopup::setFilterValue(const QString &name, const QString &value,
                                 bool enabled)
{
    auto values = this->activeFilterValues(name);
    const auto existing =
        std::ranges::find_if(values, [&value](const QString &other) {
            return other.compare(value, Qt::CaseInsensitive) == 0;
        });
    if (enabled == (existing != values.end()))
    {
        return;
    }
    if (enabled)
    {
        values.append(value);
    }
    else
    {
        values.erase(existing);
    }
    this->setFilterValues(name, values);
}

void SearchPopup::setFilterValues(const QString &name,
                                  const QStringList &values)
{
    auto text = this->searchInput_->text();
    auto match = findFilter(text, name);

    if (!match)
    {
        if (values.isEmpty())
        {
            return;
        }
        if (!text.isEmpty() && !text.endsWith(' '))
        {
            text += ' ';
        }
        text += formatFilter(name, values);
    }
    else if (values.isEmpty())
    {
        removeToken(text, match->capturedStart(), match->capturedLength());
    }
    else
    {
        // values within one filter match any of them, so merge into it
        text.replace(match->capturedStart(), match->capturedLength(),
                     formatFilter(name, values));
    }

    if (text != this->searchInput_->text())
    {
        this->setSearchText(text);
    }
}

bool SearchPopup::hasFilters() const
{
    auto it = predicateRegex().globalMatch(this->searchInput_->text());
    while (it.hasNext())
    {
        if (FILTER_NAMES.contains(it.next().captured("name")))
        {
            return true;
        }
    }
    return false;
}

void SearchPopup::clearFilters()
{
    auto text = this->searchInput_->text();

    std::vector<QRegularExpressionMatch> filters;
    auto it = predicateRegex().globalMatch(text);
    while (it.hasNext())
    {
        auto match = it.next();
        if (FILTER_NAMES.contains(match.captured("name")))
        {
            filters.push_back(match);
        }
    }

    for (auto match = filters.rbegin(); match != filters.rend(); ++match)
    {
        removeToken(text, match->capturedStart(), match->capturedLength());
    }
    this->setSearchText(text.trimmed());
}

void SearchPopup::setSearchText(const QString &text)
{
    // replace through a selection to keep undo history
    this->searchInput_->selectAll();
    this->searchInput_->insert(text);
}

std::vector<std::unique_ptr<MessagePredicate>> SearchPopup::parsePredicates(
    const QString &input)
{
    QRegularExpressionMatchIterator it = predicateRegex().globalMatch(input);

    std::vector<std::unique_ptr<MessagePredicate>> predicates;

    while (it.hasNext())
    {
        QRegularExpressionMatch match = it.next();

        QString name = match.captured("name");
        bool isNegated = !match.captured("negation").isEmpty();
        QString value = match.captured("value");
        value.remove(trimQuotationMarksRegex());

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
        else if (name == "client")
        {
            predicates.push_back(
                std::make_unique<ClientPredicate>(value, isNegated));
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

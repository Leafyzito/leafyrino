#include "widgets/dialogs/ChannelManagementDialog.hpp"

#include "Application.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/dialogs/MoltorinoDialogTheme.hpp"
#include "widgets/dialogs/PopupControlMetrics.hpp"
#include "widgets/helper/ChannelCategoryPicker.hpp"
#include "widgets/helper/Line.hpp"
#include "widgets/layout/FlowLayout.hpp"
#include "widgets/splits/SplitHeader.hpp"

#include <QAbstractItemView>
#include <QBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QCursor>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QTextCursor>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace chatterino {

namespace {

constexpr int TITLE_LIMIT = 140;
constexpr int TAG_LIMIT = 10;
constexpr int TAG_LENGTH_LIMIT = 25;
constexpr int COMMERCIAL_CONFIRMATION_SECONDS = 8;
constexpr int HEADER_SEPARATOR_HEIGHT = 8;

class NoWheelComboBox : public QComboBox
{
public:
    using QComboBox::QComboBox;

protected:
    void wheelEvent(QWheelEvent *event) override
    {
        event->ignore();
    }
};

QHash<QString, QPointer<ChannelManagementDialog>> &openDialogs()
{
    static QHash<QString, QPointer<ChannelManagementDialog>> dialogs;
    return dialogs;
}

QString dialogKey(const TwitchChannel &channel)
{
    auto key = channel.getName().trimmed().toLower();
    if (key.isEmpty())
    {
        key = channel.roomId().trimmed();
    }
    return key;
}

float contentScale(float scale)
{
    const float taper = std::clamp((scale - 1.0F) / 0.6F, 0.0F, 1.0F);
    return scale * (1.30F - taper * 0.24F);
}

int scaledSeparatorHeight(float scale)
{
    return std::max(1, int(HEADER_SEPARATOR_HEIGHT * scale));
}

QWidget *makeSection(QWidget *parent, QVBoxLayout **bodyLayout)
{
    auto *section = new QWidget(parent);
    section->setObjectName("ChannelManagementSection");
    section->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto *layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(7);

    *bodyLayout = layout;
    return section;
}

QLabel *makeFieldLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName("ChannelManagementFieldLabel");
    return label;
}

QLabel *makeHelperLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName("ChannelManagementHelper");
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    return label;
}

void setStateProperty(QWidget *widget, const char *name, bool value)
{
    if (widget->property(name).toBool() == value)
    {
        return;
    }
    widget->setProperty(name, value);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

bool contentLabelsEqual(const std::vector<ChannelManagementContentLabel> &lhs,
                        const std::vector<ChannelManagementContentLabel> &rhs)
{
    return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                      [](const auto &left, const auto &right) {
                          return left.id == right.id &&
                                 left.isEnabled == right.isEnabled;
                      });
}

struct LanguageOption {
    const char *name;
    const char *code;
};

constexpr std::array<LanguageOption, 34> LANGUAGES{{
    {"Arabic", "ar"},     {"American Sign Language", "ase"},
    {"Bulgarian", "bg"},  {"Catalan", "ca"},
    {"Chinese", "zh"},    {"Czech", "cs"},
    {"Danish", "da"},     {"Dutch", "nl"},
    {"English", "en"},    {"Finnish", "fi"},
    {"French", "fr"},     {"German", "de"},
    {"Greek", "el"},      {"Hungarian", "hu"},
    {"Indonesian", "id"}, {"Italian", "it"},
    {"Japanese", "ja"},   {"Korean", "ko"},
    {"Malay", "ms"},      {"Norwegian", "no"},
    {"Other", "other"},   {"Polish", "pl"},
    {"Portuguese", "pt"}, {"Romanian", "ro"},
    {"Russian", "ru"},    {"Slovak", "sk"},
    {"Spanish", "es"},    {"Swedish", "sv"},
    {"Tagalog", "tl"},    {"Thai", "th"},
    {"Turkish", "tr"},    {"Ukrainian", "uk"},
    {"Vietnamese", "vi"}, {"Hindi", "hi"},
}};

QString formatCooldown(int seconds)
{
    seconds = std::max(0, seconds);
    const auto minutes = seconds / 60;
    const auto remainingSeconds = seconds % 60;
    return QString("%1:%2").arg(minutes).arg(remainingSeconds, 2, 10,
                                             QChar('0'));
}

}  // namespace

void ChannelManagementDialog::showForChannel(
    const std::shared_ptr<TwitchChannel> &channel, QWidget *parent)
{
    if (!channel || channel->isEmpty())
    {
        return;
    }

    const auto key = dialogKey(*channel);
    if (auto existing = openDialogs().value(key))
    {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }

    auto *dialog = new ChannelManagementDialog(channel, parent);
    openDialogs().insert(key, dialog);
    QObject::connect(dialog, &QObject::destroyed, [key] {
        openDialogs().remove(key);
    });

    QPoint center = QCursor::pos();
    if (parent != nullptr && parent->window() != nullptr)
    {
        center = parent->window()->geometry().center();
    }

    if (auto *screen = QGuiApplication::screenAt(center))
    {
        dialog->setScreen(screen);
    }
    dialog->show();
    const auto size = dialog->size();
    dialog->showAndMoveTo(center - QPoint(size.width() / 2, size.height() / 2),
                          widgets::BoundsChecking::DesiredPosition);
    dialog->raise();
    dialog->activateWindow();
}

ChannelManagementDialog::ChannelManagementDialog(
    std::shared_ptr<TwitchChannel> channel, QWidget *parent)
    : DraggablePopup(true, parent)
    , channel_(std::move(channel))
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setObjectName("ChannelManagementDialog");
    this->setWindowTitle("Edit stream info");

    auto *container = this->getLayoutContainer();
    container->setObjectName("ChannelManagementRoot");

    container->setMouseTracking(true);
    auto *root = new QVBoxLayout(container);
    root->setContentsMargins(5, 5, 5, 5);
    root->setSpacing(0);

    this->headerWidget_ = new QWidget(container);
    this->headerWidget_->setObjectName("ChannelManagementHeader");
    auto *headerLayout = new QHBoxLayout(this->headerWidget_);
    headerLayout->setContentsMargins(4, 3, 4, 3);
    headerLayout->setSpacing(3);

    auto *headerText = new QVBoxLayout;
    headerText->setContentsMargins(0, 0, 0, 0);
    this->headerTitleLabel_ =
        new QLabel("Edit stream info", this->headerWidget_);
    this->headerTitleLabel_->setObjectName("ChannelManagementHeaderTitle");
    this->headerSubtitleLabel_ = new QLabel(
        QString("#%1  •  Loading stream info").arg(this->channel_->getName()),
        this->headerWidget_);
    this->headerSubtitleLabel_->setObjectName(
        "ChannelManagementHeaderSubtitle");
    headerText->addWidget(this->headerTitleLabel_);
    headerText->addWidget(this->headerSubtitleLabel_);
    headerLayout->addLayout(headerText, 1);

    this->pinButton_ = this->createPinButton();
    this->pinButton_->setToolTip("Pin stream editor");
    this->pinButton_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    headerLayout->addWidget(this->pinButton_);

    this->closeButton_ = new SvgButton(
        {
            .dark = ":/buttons/cancel.svg",
            .light = ":/buttons/cancelDark.svg",
        },
        this, QSize{3, 3});
    this->closeButton_->setScaleIndependentSize(18, 18);
    this->closeButton_->setToolTip("Close");
    this->closeButton_->setCursor(Qt::PointingHandCursor);
    QObject::connect(this->closeButton_, &Button::leftClicked, this,
                     &QWidget::close);
    headerLayout->addWidget(this->closeButton_);
    root->addWidget(this->headerWidget_);

    auto *separator = new Line(false);
    separator->setObjectName("ChannelManagementSeparator");
    separator->setFixedHeight(scaledSeparatorHeight(this->scale()));
    root->addWidget(separator);

    this->loadingBar_ = new QProgressBar(container);
    this->loadingBar_->setObjectName("ChannelManagementLoadingBar");
    this->loadingBar_->setTextVisible(false);
    this->loadingBar_->setRange(0, 0);
    this->loadingBar_->setFixedHeight(3);
    this->loadingBar_->setProperty("loading", true);
    root->addWidget(this->loadingBar_);

    this->scrollArea_ = new QScrollArea(container);
    this->scrollArea_->setObjectName("ChannelManagementScrollArea");
    this->scrollArea_->setFrameShape(QFrame::NoFrame);
    this->scrollArea_->setWidgetResizable(true);
    this->scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    this->scrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    this->scrollArea_->viewport()->installEventFilter(this);
    root->addWidget(this->scrollArea_, 1);

    this->contentWidget_ = new QWidget;
    this->contentWidget_->setObjectName("ChannelManagementContent");
    this->contentWidget_->installEventFilter(this);
    this->contentLayout_ = new QVBoxLayout(this->contentWidget_);
    this->contentLayout_->setContentsMargins(10, 6, 10, 8);
    this->contentLayout_->setSpacing(14);
    this->scrollArea_->setWidget(this->contentWidget_);
    QVBoxLayout *broadcastLayout = nullptr;
    auto *broadcastSection =
        makeSection(this->contentWidget_, &broadcastLayout);

    auto *titleHeader = new QHBoxLayout;
    titleHeader->setContentsMargins(0, 0, 0, 0);
    titleHeader->addWidget(makeFieldLabel("Title", broadcastSection));
    this->titleValidationLabel_ = makeHelperLabel({}, broadcastSection);
    this->titleValidationLabel_->setObjectName(
        "ChannelManagementTitleValidation");
    this->titleValidationLabel_->setWordWrap(false);
    this->titleValidationLabel_->setAlignment(Qt::AlignRight |
                                              Qt::AlignVCenter);
    this->titleValidationLabel_->setSizePolicy(QSizePolicy::Ignored,
                                               QSizePolicy::Fixed);
    titleHeader->addWidget(this->titleValidationLabel_, 1);
    this->titleCounterLabel_ = new QLabel("0/140", broadcastSection);
    this->titleCounterLabel_->setObjectName("ChannelManagementCounter");
    titleHeader->addWidget(this->titleCounterLabel_);
    broadcastLayout->addLayout(titleHeader);

    this->titleEdit_ = new QPlainTextEdit(broadcastSection);
    this->titleEdit_->setObjectName("ChannelManagementTitleInput");
    this->titleEdit_->setPlaceholderText("Tell viewers what you're streaming");
    this->titleEdit_->setTabChangesFocus(true);
    this->titleEdit_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->titleEdit_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->titleEdit_->setFixedHeight(72);
    this->titleEdit_->setAccessibleName("Title");
    broadcastLayout->addWidget(this->titleEdit_);

    auto *categoryAndLanguage = new QHBoxLayout;
    categoryAndLanguage->setObjectName("ChannelManagementFieldColumns");
    auto *categoryColumn = new QVBoxLayout;
    auto *categoryHeader = new QHBoxLayout;
    categoryHeader->addWidget(makeFieldLabel("Category", broadcastSection));
    categoryHeader->addStretch(1);
    this->categoryStatusLabel_ = new QLabel(broadcastSection);
    this->categoryStatusLabel_->setObjectName("ChannelManagementHelper");
    this->categoryStatusLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    categoryHeader->addWidget(this->categoryStatusLabel_);
    categoryColumn->addLayout(categoryHeader);
    this->categoryPicker_ = new ChannelCategoryPicker(broadcastSection);
    this->categoryPicker_->setObjectName("ChannelManagementCategoryInput");
    categoryColumn->addWidget(this->categoryPicker_);
    categoryAndLanguage->addLayout(categoryColumn, 2);

    auto *languageColumn = new QVBoxLayout;
    languageColumn->addWidget(makeFieldLabel("Language", broadcastSection));
    this->languageCombo_ = new NoWheelComboBox(broadcastSection);
    this->languageCombo_->setObjectName("ChannelManagementLanguageInput");
    this->languageCombo_->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    this->languageCombo_->setMinimumContentsLength(8);
    this->languageCombo_->setMaxVisibleItems(12);
    this->languageCombo_->setAccessibleName("Stream language");
    for (const auto &language : LANGUAGES)
    {
        this->languageCombo_->addItem(language.name, language.code);
    }
    this->languageCombo_->setCurrentIndex(-1);
    languageColumn->addWidget(this->languageCombo_);
    categoryAndLanguage->addLayout(languageColumn, 1);
    broadcastLayout->addLayout(categoryAndLanguage);
    this->categoryHintLabel_ = makeHelperLabel({}, broadcastSection);
    this->categoryHintLabel_->setObjectName("ChannelManagementCategoryHint");
    broadcastLayout->addWidget(this->categoryHintLabel_);
    this->contentLayout_->addWidget(broadcastSection);

    QVBoxLayout *discoveryLayout = nullptr;
    auto *discoverySection =
        makeSection(this->contentWidget_, &discoveryLayout);

    auto *tagsHeader = new QHBoxLayout;
    tagsHeader->setContentsMargins(0, 0, 0, 0);
    tagsHeader->addWidget(makeFieldLabel("Tags", discoverySection));
    tagsHeader->addStretch(1);
    this->tagCountLabel_ = new QLabel("0/10", discoverySection);
    this->tagCountLabel_->setObjectName("ChannelManagementCounter");
    tagsHeader->addWidget(this->tagCountLabel_);
    discoveryLayout->addLayout(tagsHeader);

    this->tagEditor_ = new QWidget(discoverySection);
    this->tagEditor_->setObjectName("ChannelManagementTagEditor");
    auto *tagEditorLayout = new QVBoxLayout(this->tagEditor_);
    tagEditorLayout->setContentsMargins(6, 6, 6, 6);
    tagEditorLayout->setSpacing(4);
    this->tagChipsWidget_ = new QWidget(this->tagEditor_);
    this->tagChipsWidget_->setObjectName("ChannelManagementTagChips");
    this->tagFlow_ = new FlowLayout(
        this->tagChipsWidget_, {.margin = 0, .hSpacing = 5, .vSpacing = 5});
    tagEditorLayout->addWidget(this->tagChipsWidget_);

    auto *tagInputRow = new QHBoxLayout;
    tagInputRow->setContentsMargins(0, 0, 0, 0);
    tagInputRow->setSpacing(6);
    this->tagInput_ = new QLineEdit(this->tagEditor_);
    this->tagInput_->setObjectName("ChannelManagementTagInput");
    this->tagInput_->setPlaceholderText("Add a tag");
    this->tagInput_->setAccessibleName("New tag");
    tagInputRow->addWidget(this->tagInput_, 1);
    this->addTagButton_ = new QPushButton("Add", this->tagEditor_);
    this->addTagButton_->setObjectName("ChannelManagementSecondaryButton");
    tagInputRow->addWidget(this->addTagButton_);
    tagEditorLayout->addLayout(tagInputRow);
    discoveryLayout->addWidget(this->tagEditor_);
    this->tagHintLabel_ = makeHelperLabel({}, discoverySection);
    this->tagHintLabel_->setObjectName("ChannelManagementTagHint");
    discoveryLayout->addWidget(this->tagHintLabel_);

    this->contentLayout_->addWidget(discoverySection);

    QVBoxLayout *classificationLayout = nullptr;
    auto *classificationSection =
        makeSection(this->contentWidget_, &classificationLayout);
    this->contentLabelsToggle_ = new QToolButton(classificationSection);
    this->contentLabelsToggle_->setObjectName(
        "ChannelManagementContentLabelsToggle");
    this->contentLabelsToggle_->setText("Content classification");
    this->contentLabelsToggle_->setToolButtonStyle(
        Qt::ToolButtonTextBesideIcon);
    this->contentLabelsToggle_->setArrowType(Qt::RightArrow);
    this->contentLabelsToggle_->setCheckable(true);
    this->contentLabelsToggle_->setSizePolicy(QSizePolicy::Expanding,
                                              QSizePolicy::Fixed);
    auto *classificationHeader = new QHBoxLayout(this->contentLabelsToggle_);
    classificationHeader->setContentsMargins(0, 0, 6, 0);
    classificationHeader->addStretch(1);
    this->contentLabelsSummary_ =
        makeHelperLabel({}, this->contentLabelsToggle_);
    this->contentLabelsSummary_->setWordWrap(false);
    this->contentLabelsSummary_->setAttribute(Qt::WA_TransparentForMouseEvents);
    classificationHeader->addWidget(this->contentLabelsSummary_);
    classificationLayout->addWidget(this->contentLabelsToggle_);
    this->contentLabelsWidget_ = new QWidget(classificationSection);
    this->contentLabelsWidget_->setObjectName("ChannelManagementContentLabels");
    auto *classificationOptions = new QVBoxLayout(this->contentLabelsWidget_);
    classificationOptions->setContentsMargins(0, 0, 0, 0);
    auto *labelsList = new QWidget(this->contentLabelsWidget_);
    this->contentLabelsLayout_ = new QVBoxLayout(labelsList);
    this->contentLabelsLayout_->setContentsMargins(0, 0, 0, 0);
    this->contentLabelsLayout_->setSpacing(4);
    classificationOptions->addWidget(labelsList);
    classificationLayout->addWidget(this->contentLabelsWidget_);
    this->contentLabelsWidget_->hide();
    QObject::connect(this->contentLabelsToggle_, &QToolButton::toggled, this,
                     [this](bool expanded) {
                         this->contentLabelsToggle_->setArrowType(
                             expanded ? Qt::DownArrow : Qt::RightArrow);
                         this->contentLabelsWidget_->setVisible(expanded);
                     });

    this->rerunCheck_ = new QCheckBox("Rerun", this->contentLabelsWidget_);
    this->rerunCheck_->setObjectName("ChannelManagementRerunCheck");
    classificationOptions->addWidget(this->rerunCheck_);
    this->contentLayout_->addWidget(classificationSection);

    QVBoxLayout *commercialLayout = nullptr;
    auto *commercialSection =
        makeSection(this->contentWidget_, &commercialLayout);
    this->commercialAvailabilityLabel_ = makeHelperLabel({}, commercialSection);
    this->commercialAvailabilityLabel_->setObjectName(
        "ChannelManagementCommercialStatus");

    auto *commercialRow = new QBoxLayout(QBoxLayout::LeftToRight);
    commercialRow->setObjectName("ChannelManagementCommercialRow");
    commercialRow->addWidget(
        makeFieldLabel("Commercial break", commercialSection));
    auto *commercialControlsWidget = new QWidget(commercialSection);
    auto *commercialControls = new QHBoxLayout(commercialControlsWidget);
    commercialControls->setContentsMargins(0, 0, 0, 0);
    commercialControls->setSpacing(6);
    commercialControls->addStretch(1);
    this->commercialLengthCombo_ =
        new NoWheelComboBox(commercialControlsWidget);
    this->commercialLengthCombo_->setObjectName(
        "ChannelManagementCommercialLength");
    for (const auto seconds : {30, 60, 90, 120, 150, 180})
    {
        this->commercialLengthCombo_->addItem(
            seconds == 60 ? QStringLiteral("1 minute")
                          : QString("%1 seconds").arg(seconds),
            seconds);
    }
    this->commercialLengthCombo_->setAccessibleName("Commercial length");
    commercialControls->addWidget(this->commercialLengthCombo_);
    this->runCommercialButton_ =
        new QPushButton("Run commercial", commercialControlsWidget);
    this->runCommercialButton_->setObjectName(
        "ChannelManagementCommercialButton");
    commercialControls->addWidget(this->runCommercialButton_);
    commercialRow->addWidget(commercialControlsWidget, 1);
    commercialLayout->addLayout(commercialRow);
    commercialLayout->addWidget(this->commercialAvailabilityLabel_);

    this->commercialConfirmationLabel_ = new QLabel(commercialSection);
    this->commercialConfirmationLabel_->setObjectName(
        "ChannelManagementCommercialConfirmation");
    this->commercialConfirmationLabel_->setWordWrap(true);
    this->commercialConfirmationLabel_->hide();
    commercialLayout->addWidget(this->commercialConfirmationLabel_);
    this->contentLayout_->addWidget(commercialSection);
    this->contentLayout_->addStretch(1);

    this->footerWidget_ = new QWidget(container);
    this->footerWidget_->setObjectName("ChannelManagementFooter");
    this->footerWidget_->installEventFilter(this);
    auto *footerLayout = new QVBoxLayout(this->footerWidget_);
    footerLayout->setContentsMargins(10, 8, 10, 9);
    footerLayout->setSpacing(7);
    this->statusLabel_ = new QLabel(this->footerWidget_);
    this->statusLabel_->setObjectName("ChannelManagementStatus");
    this->statusLabel_->setWordWrap(true);
    footerLayout->addWidget(this->statusLabel_);

    auto *footerActions = new QHBoxLayout;
    footerActions->setContentsMargins(0, 0, 0, 0);
    footerActions->setSpacing(7);
    this->cancelButton_ = new QPushButton("Cancel", this->footerWidget_);
    this->cancelButton_->setObjectName("ChannelManagementSecondaryButton");
    this->cancelButton_->setSizePolicy(QSizePolicy::Expanding,
                                       QSizePolicy::Fixed);
    footerActions->addWidget(this->cancelButton_, 1);
    this->saveButton_ = new QPushButton("Save", this->footerWidget_);
    this->saveButton_->setObjectName("ChannelManagementPrimaryButton");
    this->saveButton_->setSizePolicy(QSizePolicy::Expanding,
                                     QSizePolicy::Fixed);
    footerActions->addWidget(this->saveButton_, 1);
    footerLayout->addLayout(footerActions);
    root->addWidget(this->footerWidget_);

    this->commercialConfirmationTimer_.setSingleShot(true);
    this->commercialConfirmationTimer_.setInterval(
        COMMERCIAL_CONFIRMATION_SECONDS * 1000);
    this->commercialCooldownTimer_.setInterval(1000);

    QObject::connect(this->titleEdit_, &QPlainTextEdit::textChanged, this,
                     [this] {
                         this->updateTitleCounter();
                         this->updateControls();
                     });

    QObject::connect(this->categoryPicker_,
                     &ChannelCategoryPicker::selectionChanged, this, [this] {
                         this->updateControls();
                     });
    QObject::connect(this->categoryPicker_,
                     &ChannelCategoryPicker::statusChanged, this,
                     &ChannelManagementDialog::setCategoryHint);
    QObject::connect(this->tagInput_, &QLineEdit::textChanged, this, [this] {
        this->updateControls();
    });
    QObject::connect(this->tagInput_, &QLineEdit::returnPressed, this, [this] {
        this->addTagFromInput();
    });
    QObject::connect(this->addTagButton_, &QPushButton::clicked, this, [this] {
        this->addTagFromInput();
    });
    QObject::connect(this->languageCombo_,
                     qOverload<int>(&QComboBox::activated), this, [this] {
                         this->updateControls();
                     });
    QObject::connect(this->rerunCheck_, &QCheckBox::toggled, this, [this] {
        this->updateControls();
    });

    QObject::connect(this->commercialLengthCombo_,
                     qOverload<int>(&QComboBox::activated), this, [this] {
                         this->resetCommercialConfirmation();
                         this->setCommercialMessage({});
                         this->updateCommercialAvailability();
                     });
    QObject::connect(this->runCommercialButton_, &QPushButton::clicked, this,
                     [this] {
                         this->handleCommercialClick();
                     });
    QObject::connect(&this->commercialConfirmationTimer_, &QTimer::timeout,
                     this, [this] {
                         this->resetCommercialConfirmation();
                         this->updateCommercialAvailability();
                     });
    QObject::connect(&this->commercialCooldownTimer_, &QTimer::timeout, this,
                     [this] {
                         this->updateCommercialAvailability();
                         this->updateControls();
                     });

    QObject::connect(this->cancelButton_, &QPushButton::clicked, this,
                     &QWidget::close);
    QObject::connect(this->saveButton_, &QPushButton::clicked, this, [this] {
        this->saveMetadata();
    });
    auto *saveShortcut = new QShortcut(QKeySequence("Ctrl+Return"), this);
    QObject::connect(saveShortcut, &QShortcut::activated, this, [this] {
        if (this->saveButton_->isEnabled())
        {
            this->saveMetadata();
        }
    });
    this->saveButton_->setToolTip("Save changes (Ctrl+Enter)");

    this->streamStatusConnection_ =
        this->channel_->streamStatusChanged.connect([this] {
            if (!this->channel_->isLive())
            {
                this->resetCommercialConfirmation();
            }
            this->setCommercialMessage({});
            this->updateCommercialAvailability();
            this->updateControls();
        });

    for (auto *label : this->findChildren<QLabel *>())
    {
        label->setTextFormat(Qt::PlainText);
    }
    this->updateTitleCounter();
    this->refreshStyle();
    this->loadState();
}

void ChannelManagementDialog::loadState()
{
    this->categoryPicker_->stopSearch();
    this->loadInFlight_ = true;
    this->loaded_ = false;
    this->prepareLoadingState();
    this->setLoadingVisual(true);
    this->updateControls();
    this->updateCommercialAvailability();

    QPointer<ChannelManagementDialog> self(this);
    ChannelManagement::loadState(
        this->channel_,
        [self](ChannelManagementState state) {
            if (!self)
            {
                return;
            }
            self->loadInFlight_ = false;
            self->loaded_ = true;
            self->applyState(std::move(state));
            self->setStatus({});
            self->setLoadingVisual(false);
            self->updateCommercialAvailability();
            self->updateControls();
        },
        [self](const QString &) {
            if (!self)
            {
                return;
            }
            self->refreshParentHeaderIcons();
            self->close();
        });
}

void ChannelManagementDialog::prepareLoadingState()
{
    this->setStatus({});
    this->setCategoryHint({});
    this->setCommercialMessage({});
    this->headerSubtitleLabel_->setText(
        QString("#%1  •  Loading stream info").arg(this->channel_->getName()));

    const auto streamStatus = this->channel_->accessStreamStatus();
    {
        const QSignalBlocker blocker(this->titleEdit_);
        this->titleEdit_->setPlainText(streamStatus->title.trimmed());
        this->titleEdit_->setPlaceholderText("Loading stream title");
    }
    this->updateTitleCounter();

    this->originalMetadata_ = {};
    this->stagedTags_.clear();
    this->categoryPicker_->setCategory({});
    this->categoryPicker_->setText(streamStatus->game.trimmed());
    this->categoryPicker_->setPlaceholderText("Loading category");

    this->rebuildTagChips();
    this->rebuildContentLabels();
    {
        const QSignalBlocker blocker(this->languageCombo_);
        this->languageCombo_->setCurrentIndex(-1);
    }
}

void ChannelManagementDialog::setLoadingVisual(bool loading)
{
    this->loadingBar_->setProperty("loading", loading);
    this->loadingBar_->setRange(0, loading ? 0 : 1);
    if (!loading)
    {
        this->loadingBar_->setValue(0);
    }
    this->loadingBar_->style()->unpolish(this->loadingBar_);
    this->loadingBar_->style()->polish(this->loadingBar_);
}

void ChannelManagementDialog::applyState(ChannelManagementState state)
{
    this->originalMetadata_ = std::move(state.metadata);
    this->stagedTags_ = this->originalMetadata_.tags;

    this->refreshParentHeaderIcons();

    this->headerSubtitleLabel_->setText(
        QString("#%1  •  %2 access")
            .arg(this->channel_->getName(),
                 state.access == ChannelManagementAccess::Broadcaster
                     ? QStringLiteral("Broadcaster")
                     : QStringLiteral("Editor")));

    {
        const QSignalBlocker blocker(this->titleEdit_);
        this->titleEdit_->setPlaceholderText(
            "Tell viewers what you're streaming");
        this->titleEdit_->setPlainText(this->originalMetadata_.title);
    }
    this->updateTitleCounter();

    this->categoryPicker_->setCategory(this->originalMetadata_.category);
    this->categoryPicker_->setPlaceholderText("Search categories");
    this->setCategoryHint({});

    this->rebuildTagChips();

    auto languageIndex = -1;
    for (auto i = 0; i < this->languageCombo_->count(); ++i)
    {
        if (this->languageCombo_->itemData(i).toString().compare(
                this->originalMetadata_.language, Qt::CaseInsensitive) == 0)
        {
            languageIndex = i;
            break;
        }
    }
    if (languageIndex < 0 && !this->originalMetadata_.language.isEmpty())
    {
        this->languageCombo_->addItem(
            this->originalMetadata_.language.toUpper(),
            this->originalMetadata_.language.toLower());
        languageIndex = this->languageCombo_->count() - 1;
    }
    {
        const QSignalBlocker blocker(this->languageCombo_);
        this->languageCombo_->setCurrentIndex(languageIndex);
    }

    this->rebuildContentLabels();
    {
        const QSignalBlocker blocker(this->rerunCheck_);
        this->rerunCheck_->setChecked(this->originalMetadata_.isRerun);
    }
    this->contentLabelsToggle_->setChecked(
        this->originalMetadata_.isRerun ||
        std::any_of(this->originalMetadata_.contentLabels.cbegin(),
                    this->originalMetadata_.contentLabels.cend(),
                    [](const auto &label) {
                        return label.isEnabled;
                    }));
    this->rerunCheck_->setToolTip(
        this->originalMetadata_.canEditRerun
            ? QStringLiteral("Previously recorded stream")
            : QStringLiteral("Could not load Twitch's rerun setting."));
}

void ChannelManagementDialog::refreshParentHeaderIcons() const
{
    if (auto *parent = this->parentWidget())
    {
        if (auto *header = parent->findChild<SplitHeader *>(
                QString(), Qt::FindDirectChildrenOnly))
        {
            header->updateIcons();
        }
    }
}

void ChannelManagementDialog::saveMetadata()
{
    if (!this->loaded_ || this->loadInFlight_ || this->saveInFlight_ ||
        this->commercialInFlight_)
    {
        return;
    }
    if (this->hasInvalidTitle())
    {
        this->updateTitleCounter();
        this->titleEdit_->setFocus();
        return;
    }
    if (this->hasInvalidCategoryText())
    {
        this->setStatus(
            "Choose a Twitch category from the search results before saving",
            true);
        this->categoryPicker_->setFocus();
        return;
    }

    auto update = this->buildUpdate();
    if (update.empty())
    {
        return;
    }

    this->categoryPicker_->stopSearch();
    this->saveInFlight_ = true;
    this->setStatus("Saving stream info");
    this->updateControls();

    QPointer<ChannelManagementDialog> self(this);
    ChannelManagement::updateMetadata(
        this->channel_, std::move(update),
        [self] {
            if (!self)
            {
                return;
            }
            self->saveInFlight_ = false;
            self->setStatus("Stream information updated");
            self->close();
        },
        [self](const QString &error) {
            if (!self)
            {
                return;
            }

            self->saveInFlight_ = false;
            self->setStatus(error, true);
            self->updateControls();
        });
}

void ChannelManagementDialog::addTagFromInput()
{
    if (!this->loaded_ || this->saveInFlight_ || this->commercialInFlight_)
    {
        return;
    }
    const auto tag = this->tagInput_->text().trimmed();
    if (!ChannelManagement::isValidTag(tag) ||
        this->stagedTags_.size() >= TAG_LIMIT ||
        this->stagedTags_.contains(tag, Qt::CaseInsensitive))
    {
        this->updateControls();
        return;
    }

    this->stagedTags_.push_back(tag);
    this->tagInput_->clear();
    this->setStatus({});
    this->rebuildTagChips();
    this->updateControls();
    this->tagInput_->setFocus();
}

void ChannelManagementDialog::removeTag(const QString &tag)
{
    if (!this->loaded_ || this->loadInFlight_ || this->saveInFlight_ ||
        this->commercialInFlight_)
    {
        return;
    }
    for (auto i = 0; i < this->stagedTags_.size(); ++i)
    {
        if (this->stagedTags_[i].compare(tag, Qt::CaseInsensitive) == 0)
        {
            this->stagedTags_.removeAt(i);
            break;
        }
    }
    this->rebuildTagChips();
    this->updateControls();
    this->tagInput_->setFocus();
}

void ChannelManagementDialog::rebuildTagChips()
{
    QWidget *previous = this->languageCombo_;
    while (auto *item = this->tagFlow_->takeAt(0))
    {
        if (auto *widget = item->widget())
        {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }

    if (!this->loaded_)
    {
        auto *placeholder = new QLabel("Loading tags", this->tagChipsWidget_);
        placeholder->setObjectName("ChannelManagementPlaceholder");
        this->tagFlow_->addWidget(placeholder);
    }
    else if (!this->stagedTags_.isEmpty())
    {
        for (const auto &tag : std::as_const(this->stagedTags_))
        {
            auto *chip = new QWidget(this->tagChipsWidget_);
            chip->setObjectName("ChannelManagementTagChip");
            chip->setFont(this->contentWidget_->font());
            auto *layout = new QHBoxLayout(chip);
            layout->setContentsMargins(int(6 * this->scale()), 0, 0, 0);
            layout->setSpacing(int(3 * this->scale()));
            auto *label = new QLabel(tag, chip);
            label->setTextFormat(Qt::PlainText);
            label->setObjectName("ChannelManagementTagText");
            label->setFont(this->contentWidget_->font());
            layout->addWidget(label);
            auto *remove = new QPushButton(QStringLiteral("×"), chip);
            remove->setObjectName("ChannelManagementRemoveTag");
            remove->setFont(this->contentWidget_->font());
            remove->setFixedSize(int(22 * this->scale()),
                                 int(24 * this->scale()));
            remove->setCursor(Qt::PointingHandCursor);
            remove->setAccessibleName(QString("Remove %1").arg(tag));
            remove->setToolTip(remove->accessibleName());
            layout->addWidget(remove);
            QWidget::setTabOrder(previous, remove);
            previous = remove;
            QObject::connect(remove, &QPushButton::clicked, this, [this, tag] {
                this->removeTag(tag);
            });
            this->tagFlow_->addWidget(chip);
        }
    }
    this->tagChipsWidget_->setVisible(!this->loaded_ ||
                                      !this->stagedTags_.isEmpty());
    this->tagCountLabel_->setText(
        QString("%1/%2").arg(this->stagedTags_.size()).arg(TAG_LIMIT));
    this->tagFlow_->invalidate();
    this->tagChipsWidget_->updateGeometry();
    QWidget::setTabOrder(previous, this->tagInput_);
    QWidget::setTabOrder(this->tagInput_, this->addTagButton_);
    QWidget::setTabOrder(this->addTagButton_, this->contentLabelsToggle_);
}

void ChannelManagementDialog::rebuildContentLabels()
{
    QWidget *previous = this->contentLabelsToggle_;
    this->contentLabelChecks_.clear();
    while (auto *item = this->contentLabelsLayout_->takeAt(0))
    {
        if (auto *widget = item->widget())
        {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }

    if (!this->loaded_)
    {
        auto *placeholder =
            new QLabel("Loading content labels",
                       this->contentLabelsLayout_->parentWidget());
        placeholder->setObjectName("ChannelManagementPlaceholder");
        this->contentLabelsLayout_->addWidget(placeholder);
        return;
    }

    if (this->originalMetadata_.contentLabels.empty())
    {
        this->contentLabelsLayout_->addWidget(
            makeHelperLabel("No content labels are available for this channel",
                            this->contentLabelsLayout_->parentWidget()));
        return;
    }

    for (const auto &label : this->originalMetadata_.contentLabels)
    {
        auto *row = new QWidget(this->contentLabelsLayout_->parentWidget());
        row->setObjectName("ChannelManagementContentLabelRow");
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(int(6 * this->scale()));

        auto *check = new QCheckBox(label.name, row);
        check->setFont(this->contentWidget_->font());
        check->ensurePolished();
        check->setPalette(this->rerunCheck_->palette());
        check->setChecked(label.isEnabled);
        check->setEnabled(label.isSelectable && !label.isLocked);
        layout->addWidget(check, 1);

        const auto isAutomatic =
            label.id.compare("MatureGame", Qt::CaseInsensitive) == 0;
        QString state;
        if (isAutomatic)
        {
            state = QStringLiteral("Automatic");
        }
        else if (label.isLocked)
        {
            state = QStringLiteral("Locked");
        }
        else if (!label.isSelectable)
        {
            state = QStringLiteral("Read only");
        }
        if (!state.isEmpty())
        {
            auto *stateLabel = new QLabel(state, row);
            stateLabel->setObjectName("ChannelManagementStatePill");
            stateLabel->setFont(this->contentWidget_->font());
            layout->addWidget(stateLabel);
        }

        auto tooltip = label.description.trimmed();
        if (isAutomatic)
        {
            const auto automaticText = QStringLiteral(
                "Twitch controls this label automatically from the selected "
                "category");
            tooltip = tooltip.isEmpty()
                          ? automaticText
                          : tooltip + QStringLiteral("\n\n") + automaticText;
        }
        else if (label.isLocked)
        {
            const auto lockText =
                label.lockedUntil.isEmpty()
                    ? QStringLiteral("Twitch has locked this label")
                    : QString("Locked by Twitch until %1")
                          .arg(label.lockedUntil);
            tooltip = tooltip.isEmpty()
                          ? lockText
                          : tooltip + QStringLiteral("\n\n") + lockText;
        }
        else if (!label.isSelectable)
        {
            const auto readOnlyText =
                QStringLiteral("Twitch marks this label as read only");
            tooltip = tooltip.isEmpty()
                          ? readOnlyText
                          : tooltip + QStringLiteral("\n\n") + readOnlyText;
        }
        row->setToolTip(tooltip);
        check->setToolTip(tooltip);
        row->setAccessibleDescription(tooltip);
        check->setAccessibleDescription(tooltip);
        row->setProperty("restricted", label.isLocked || !label.isSelectable);
        this->contentLabelsLayout_->addWidget(row);
        this->contentLabelChecks_.insert(label.id, check);
        QWidget::setTabOrder(previous, check);
        previous = check;
        QObject::connect(check, &QCheckBox::toggled, this, [this] {
            this->updateControls();
        });
    }
    QWidget::setTabOrder(previous, this->rerunCheck_);
}

void ChannelManagementDialog::handleCommercialClick()
{
    if (!this->loaded_ || this->loadInFlight_ || this->saveInFlight_ ||
        this->commercialInFlight_ || !this->channel_->isLive() ||
        ChannelManagement::commercialCooldownRemainingSeconds(
            this->channel_->roomId()) > 0)
    {
        return;
    }

    if (!this->commercialConfirmationArmed_)
    {
        this->commercialConfirmationArmed_ = true;
        this->commercialConfirmationTimer_.start();
        this->updateCommercialAvailability();
        return;
    }

    this->startCommercial();
}

void ChannelManagementDialog::startCommercial()
{
    if (!this->loaded_ || this->loadInFlight_ || this->saveInFlight_ ||
        this->commercialInFlight_ || !this->channel_->isLive())
    {
        return;
    }

    const auto length = this->commercialLengthCombo_->currentData().toInt();
    if (!ChannelManagement::isValidCommercialLength(length))
    {
        this->setCommercialMessage("Choose a valid commercial length", true);
        this->updateCommercialAvailability();
        return;
    }

    this->resetCommercialConfirmation();
    this->setCommercialMessage({});
    this->commercialInFlight_ = true;
    this->updateCommercialAvailability();
    this->updateControls();

    QPointer<ChannelManagementDialog> self(this);
    ChannelManagement::startCommercial(
        this->channel_, length, ChannelManagementCommercialTrigger::QuickAction,
        [self](ChannelManagementCommercialResult result) {
            if (!self)
            {
                return;
            }
            self->commercialInFlight_ = false;
            if (result.retryAfterSeconds > 0)
            {
                self->setCommercialMessage({});
            }
            else
            {
                self->setCommercialMessage(
                    QString("Commercial started for %1 seconds")
                        .arg(result.lengthSeconds));
            }
            self->updateCommercialAvailability();
            self->updateControls();
        },
        [self](ChannelManagementCommercialFailure failure) {
            if (!self)
            {
                return;
            }
            self->commercialInFlight_ = false;
            if (failure.retryAfterSeconds > 0)
            {
                self->setCommercialMessage({});
            }
            else
            {
                self->setCommercialMessage(failure.message, true);
            }
            self->updateCommercialAvailability();
            self->updateControls();
        });
}

void ChannelManagementDialog::resetCommercialConfirmation()
{
    this->commercialConfirmationArmed_ = false;
    this->commercialConfirmationTimer_.stop();
    this->commercialConfirmationLabel_->hide();
}

void ChannelManagementDialog::setCommercialMessage(const QString &text,
                                                   bool error)
{
    this->commercialMessage_ = text;
    this->commercialMessageIsError_ = error;
}

void ChannelManagementDialog::updateCommercialAvailability()
{
    const auto cooldownSeconds =
        ChannelManagement::commercialCooldownRemainingSeconds(
            this->channel_->roomId());

    QString statusText;
    auto statusIsError = false;
    if (!this->loaded_)
    {
        statusText = QStringLiteral("Loading commercial controls");
    }
    else if (this->commercialInFlight_)
    {
        statusText = QStringLiteral("Starting commercial");
    }
    else if (this->channel_->isLive() && cooldownSeconds > 0)
    {
        statusText = QString("Available again in %1")
                         .arg(formatCooldown(cooldownSeconds));
    }
    else if (!this->commercialMessage_.isEmpty())
    {
        statusText = this->commercialMessage_;
        statusIsError = this->commercialMessageIsError_;
    }
    this->commercialAvailabilityLabel_->setText(statusText);
    this->commercialAvailabilityLabel_->setVisible(!statusText.isEmpty());
    this->runCommercialButton_->setToolTip(
        this->loaded_ && !this->channel_->isLive()
            ? QStringLiteral("Available when the channel is live")
            : QString());
    setStateProperty(this->commercialAvailabilityLabel_, "error",
                     statusIsError);

    if (this->loaded_ && this->channel_->isLive())
    {
        if (!this->commercialCooldownTimer_.isActive())
        {
            this->commercialCooldownTimer_.start();
        }
    }
    else
    {
        this->commercialCooldownTimer_.stop();
    }

    if (this->commercialConfirmationArmed_)
    {
        const auto length = this->commercialLengthCombo_->currentData().toInt();
        this->commercialConfirmationLabel_->setText(
            QString("Run a commercial for %1 seconds in #%2?")
                .arg(length)
                .arg(this->channel_->getName()));
        this->commercialConfirmationLabel_->show();
        this->runCommercialButton_->setText("Confirm commercial");
    }
    else
    {
        this->commercialConfirmationLabel_->hide();
        this->runCommercialButton_->setText("Run commercial");
    }
}

void ChannelManagementDialog::updateTitleCounter()
{
    const auto text = this->titleEdit_->toPlainText();
    const auto size = text.size();
    const auto overflow = size > TITLE_LIMIT;
    const auto empty = this->loaded_ && text.trimmed().isEmpty();
    this->titleCounterLabel_->setText(
        QString("%1/%2").arg(size).arg(TITLE_LIMIT));
    setStateProperty(this->titleCounterLabel_, "invalid", overflow);
    setStateProperty(this->titleEdit_, "invalid", overflow || empty);
    this->titleValidationLabel_->setText(
        overflow ? QString("Shorten by %1 character%2")
                       .arg(size - TITLE_LIMIT)
                       .arg(size - TITLE_LIMIT == 1 ? "" : "s")
        : empty ? QStringLiteral("Enter a title")
                : QString());
    this->titleValidationLabel_->setToolTip(
        this->titleValidationLabel_->text());

    QList<QTextEdit::ExtraSelection> selections;
    if (overflow)
    {
        QTextEdit::ExtraSelection excess;
        excess.cursor = this->titleEdit_->textCursor();

        const auto start = text.at(TITLE_LIMIT).isLowSurrogate() &&
                                   text.at(TITLE_LIMIT - 1).isHighSurrogate()
                               ? TITLE_LIMIT - 1
                               : TITLE_LIMIT;
        excess.cursor.setPosition(start);
        excess.cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        const auto danger =
            this->theme->isLightTheme() ? QColor("#b3261e") : QColor("#e56a6a");
        excess.format.setForeground(danger);
        selections.push_back(excess);
    }
    this->titleEdit_->setExtraSelections(selections);
}

void ChannelManagementDialog::updateControls()
{
    const auto editable = this->loaded_ && !this->loadInFlight_ &&
                          !this->saveInFlight_ && !this->commercialInFlight_;
    this->titleEdit_->setEnabled(editable);
    this->categoryPicker_->setEnabled(editable);
    this->contentLabelsToggle_->setEnabled(editable);
    this->tagInput_->setEnabled(editable &&
                                this->stagedTags_.size() < TAG_LIMIT);
    this->languageCombo_->setEnabled(editable);
    this->rerunCheck_->setEnabled(editable &&
                                  this->originalMetadata_.canEditRerun);
    for (auto *check : std::as_const(this->contentLabelChecks_))
    {
        const auto original = std::find_if(
            this->originalMetadata_.contentLabels.cbegin(),
            this->originalMetadata_.contentLabels.cend(),
            [check, this](const ChannelManagementContentLabel &label) {
                return this->contentLabelChecks_.value(label.id) == check;
            });
        const auto writable =
            original != this->originalMetadata_.contentLabels.cend() &&
            original->isSelectable && !original->isLocked;
        check->setEnabled(editable && writable);
    }

    const auto candidateTag = this->tagInput_->text().trimmed();
    const auto duplicate =
        this->stagedTags_.contains(candidateTag, Qt::CaseInsensitive);
    this->addTagButton_->setEnabled(
        editable && this->stagedTags_.size() < TAG_LIMIT && !duplicate &&
        ChannelManagement::isValidTag(candidateTag));
    for (auto *chip : this->tagChipsWidget_->findChildren<QWidget *>(
             "ChannelManagementTagChip", Qt::FindDirectChildrenOnly))
    {
        chip->setEnabled(editable);
    }

    this->saveButton_->setEnabled(editable && !this->hasInvalidTitle() &&
                                  !this->hasInvalidCategoryText() &&
                                  !this->buildUpdate().empty());
    const auto enabledLabels =
        int(this->rerunCheck_->isChecked()) +
        std::count_if(this->contentLabelChecks_.cbegin(),
                      this->contentLabelChecks_.cend(), [](auto *check) {
                          return check->isChecked();
                      });
    this->contentLabelsSummary_->setText(
        enabledLabels == 0 ? QStringLiteral("None selected")
                           : QString("%1 selected").arg(enabledLabels));
    QString tagHint;
    bool invalidTag = false;
    if (this->stagedTags_.size() >= TAG_LIMIT)
    {
        tagHint = "Remove a tag to add another";
    }
    else if (duplicate && !candidateTag.isEmpty())
    {
        tagHint = "This tag is already added";
        invalidTag = true;
    }
    else if (candidateTag.size() > TAG_LENGTH_LIMIT)
    {
        tagHint = "Tags can have up to 25 characters";
        invalidTag = true;
    }
    else if (!candidateTag.isEmpty() &&
             !ChannelManagement::isValidTag(candidateTag))
    {
        tagHint = "Use letters or numbers without spaces";
        invalidTag = true;
    }
    this->tagHintLabel_->setText(tagHint);
    this->tagHintLabel_->setVisible(!tagHint.isEmpty());
    setStateProperty(this->tagHintLabel_, "error", invalidTag);
    this->cancelButton_->setEnabled(!this->saveInFlight_ &&
                                    !this->commercialInFlight_);
    this->closeButton_->setEnabled(!this->saveInFlight_ &&
                                   !this->commercialInFlight_);

    const auto cooldownActive =
        ChannelManagement::commercialCooldownRemainingSeconds(
            this->channel_->roomId()) > 0;
    const auto canRunCommercial = this->loaded_ && this->channel_->isLive() &&
                                  !this->saveInFlight_ &&
                                  !this->commercialInFlight_ && !cooldownActive;
    this->commercialLengthCombo_->setEnabled(canRunCommercial);
    this->runCommercialButton_->setEnabled(canRunCommercial);
}

void ChannelManagementDialog::setStatus(const QString &text, bool error)
{
    this->statusText_ = text;
    this->statusIsError_ = error;
    this->statusLabel_->setText(text);
    this->statusLabel_->setVisible(!text.isEmpty());
    setStateProperty(this->statusLabel_, "error", error);
}

void ChannelManagementDialog::setCategoryHint(const QString &text, bool error)
{
    this->categoryHintText_ = text;
    this->categoryHintIsError_ = error;
    this->categoryStatusLabel_->setText(error ? QString() : text);
    this->categoryHintLabel_->setText(text);
    this->categoryHintLabel_->setVisible(error && !text.isEmpty());
    setStateProperty(this->categoryHintLabel_, "error", error);
}

ChannelManagementUpdate ChannelManagementDialog::buildUpdate() const
{
    ChannelManagementUpdate update;
    auto title = this->titleEdit_->toPlainText().trimmed();
    title.replace('\r', ' ');
    title.replace('\n', ' ');
    if (title != this->originalMetadata_.title)
    {
        update.title = title;
    }

    if (this->categoryPicker_->selection().id !=
        this->originalMetadata_.category.id)
    {
        update.category = this->categoryPicker_->selection();
    }

    const auto language = this->languageCombo_->currentData().toString();
    if (!language.isEmpty() &&
        language.compare(this->originalMetadata_.language,
                         Qt::CaseInsensitive) != 0)
    {
        update.language = language;
    }

    if (this->stagedTags_ != this->originalMetadata_.tags)
    {
        update.tags = this->stagedTags_;
    }

    auto labels = this->currentContentLabels();
    if (!contentLabelsEqual(labels, this->originalMetadata_.contentLabels))
    {
        update.contentLabels = std::move(labels);
    }

    if (this->originalMetadata_.canEditRerun &&
        this->rerunCheck_->isChecked() != this->originalMetadata_.isRerun)
    {
        update.isRerun = this->rerunCheck_->isChecked();
    }
    return update;
}

std::vector<ChannelManagementContentLabel>
    ChannelManagementDialog::currentContentLabels() const
{
    auto labels = this->originalMetadata_.contentLabels;
    for (auto &label : labels)
    {
        if (auto *check = this->contentLabelChecks_.value(label.id))
        {
            label.isEnabled = check->isChecked();
        }
    }
    return labels;
}

bool ChannelManagementDialog::hasInvalidCategoryText() const
{
    return !this->categoryPicker_->hasValidSelection();
}

bool ChannelManagementDialog::hasInvalidTitle() const
{
    const auto title = this->titleEdit_->toPlainText();
    return title.trimmed().isEmpty() || title.size() > TITLE_LIMIT;
}

void ChannelManagementDialog::refreshStyle()
{
    if (!this->theme || !this->headerWidget_)
    {
        return;
    }

    const auto rawScale = this->scale();
    const auto effectiveScale = contentScale(rawScale);

    const auto radius = std::max(1, int(2 * rawScale));
    const auto paddingX = std::max(4, int(5 * effectiveScale));
    const auto paddingY = 0;
    const auto controlMinHeight = popupControlMinimumHeight(effectiveScale);
    const auto uiFont =
        getApp()->getFonts()->getFont(FontStyle::UiMedium, effectiveScale);
    const auto uiBoldFont =
        getApp()->getFonts()->getFont(FontStyle::UiMediumBold, effectiveScale);
    const QFontMetrics uiMetrics(uiFont);
    const auto inputHeight = popupControlHeight(uiFont, effectiveScale);
    const auto controlHeight = popupControlHeight(uiBoldFont, effectiveScale);
    const auto rowSpacing = std::max(1, int(3 * effectiveScale));

    auto background = this->theme->window.background;
    auto card = this->theme->splits.header.background;
    auto input = this->theme->splits.input.background;
    auto textColor = this->theme->window.text;
    auto muted = textColor;
    muted.setAlpha(160);
    const auto border = this->theme->splits.header.border;
    const auto focusBorder = this->theme->accent;
    auto accent = this->theme->tabs.selected.backgrounds.regular;
    auto accentText = this->theme->tabs.selected.text;
    auto hover =
        this->theme->isLightTheme() ? card.darker(104) : card.lighter(108);
    const auto danger =
        this->theme->isLightTheme() ? QColor("#b3261e") : QColor("#e56a6a");

    this->closeButton_->setColor(textColor);
    this->headerTitleLabel_->setFont(uiBoldFont);
    this->headerSubtitleLabel_->setFont(uiFont);

    if (auto *layout =
            qobject_cast<QHBoxLayout *>(this->headerWidget_->layout()))
    {
        layout->setContentsMargins(
            std::max(2, int(4 * rawScale)), std::max(2, int(3 * rawScale)),
            std::max(2, int(4 * rawScale)), std::max(2, int(3 * rawScale)));
        layout->setSpacing(std::max(2, int(3 * rawScale)));
    }
    if (auto *layout =
            qobject_cast<QVBoxLayout *>(this->getLayoutContainer()->layout()))
    {
        const auto margin = std::max(3, int(5 * rawScale));
        layout->setContentsMargins(margin, margin, margin, margin);
    }
    if (auto *separator =
            this->findChild<QWidget *>("ChannelManagementSeparator"))
    {
        separator->setFixedHeight(scaledSeparatorHeight(rawScale));
    }
    this->loadingBar_->setFixedHeight(std::max(2, int(3 * rawScale)));
    this->titleEdit_->setFixedHeight(std::max(60, int(64 * rawScale)));
    this->titleValidationLabel_->setFixedHeight(uiMetrics.height());
    this->titleValidationLabel_->setContentsMargins(0, 0, int(6 * rawScale), 0);
    this->contentLabelsToggle_->setFixedHeight(controlHeight);
    this->categoryStatusLabel_->setMinimumHeight(uiMetrics.height());
    this->categoryPicker_->setFixedHeight(inputHeight);
    this->tagInput_->setFixedHeight(inputHeight);
    this->languageCombo_->setFixedHeight(controlHeight);
    this->commercialLengthCombo_->setFixedHeight(controlHeight);
    this->contentLayout_->setContentsMargins(
        int(4 * rawScale), int(4 * rawScale), int(4 * rawScale),
        int(4 * rawScale));
    const auto sectionSpacing = int(10 * effectiveScale);
    this->contentLayout_->setSpacing(sectionSpacing);
    for (auto *layout : this->contentWidget_->findChildren<QBoxLayout *>())
    {
        if (layout != this->contentLayout_)
        {
            layout->setSpacing(rowSpacing);
            if (layout->objectName() ==
                QStringLiteral("ChannelManagementFieldColumns"))
            {
                layout->setSpacing(int(12 * rawScale));
                layout->setContentsMargins(0, sectionSpacing - rowSpacing, 0,
                                           0);
            }
        }
    }
    this->tagEditor_->layout()->setContentsMargins(
        int(4 * effectiveScale), int(4 * effectiveScale),
        int(4 * effectiveScale), int(4 * effectiveScale));
    for (auto *chip : this->tagChipsWidget_->findChildren<QWidget *>(
             "ChannelManagementTagChip", Qt::FindDirectChildrenOnly))
    {
        chip->layout()->setContentsMargins(int(6 * rawScale), 0, 0, 0);
        chip->layout()->setSpacing(int(3 * rawScale));
        chip->findChild<QPushButton *>()->setFixedSize(controlHeight,
                                                       controlHeight);
    }
    this->tagFlow_->setHorizontalSpacing(int(5 * rawScale));
    this->tagFlow_->setVerticalSpacing(int(5 * rawScale));
    if (auto *layout = this->footerWidget_->layout())
    {
        layout->setContentsMargins(int(4 * rawScale), int(4 * effectiveScale),
                                   int(4 * rawScale), int(4 * rawScale));
        layout->setSpacing(rowSpacing);
        for (auto *row : this->footerWidget_->findChildren<QHBoxLayout *>())
        {
            row->setSpacing(rowSpacing);
        }
    }
    this->tagChipsWidget_->setMinimumHeight(0);
    this->contentLabelsWidget_->setMinimumHeight(0);
    this->cancelButton_->setMinimumWidth(int(60 * effectiveScale));
    this->saveButton_->setMinimumWidth(int(60 * effectiveScale));
    for (auto *button : this->findChildren<QPushButton *>())
    {
        const auto name = button->objectName();
        if (name == QStringLiteral("ChannelManagementSecondaryButton") ||
            name == QStringLiteral("ChannelManagementCommercialButton") ||
            name == QStringLiteral("ChannelManagementPrimaryButton"))
        {
            button->setFixedHeight(controlHeight);
        }
    }

    auto style = QStringLiteral(R"(
        QWidget#ChannelManagementRoot {
            background: %1;
        }
        QWidget#ChannelManagementContent,
        QScrollArea#ChannelManagementScrollArea,
        QScrollArea#ChannelManagementScrollArea > QWidget > QWidget {
            background: %1;
            border: none;
        }
        QWidget#ChannelManagementHeader,
        QWidget#ChannelManagementFooter {
            background: %1;
        }
        QProgressBar#ChannelManagementLoadingBar {
            background: transparent;
            border: none;
        }
        QProgressBar#ChannelManagementLoadingBar::chunk {
            background: %7;
            border: none;
        }
        QProgressBar#ChannelManagementLoadingBar[loading="false"]::chunk {
            background: transparent;
        }
        QLabel#ChannelManagementHeaderTitle {
            color: %4;
        }
        QLabel#ChannelManagementFieldLabel,
        QLabel#ChannelManagementHeaderSubtitle,
        QLabel#ChannelManagementHelper,
        QLabel#ChannelManagementPlaceholder,
        QLabel#ChannelManagementCounter,
        QLabel#ChannelManagementCommercialStatus,
        QLabel#ChannelManagementCategoryHint,
        QLabel#ChannelManagementTagHint,
        QLabel#ChannelManagementStatus {
            color: %5;
        }
        QLabel#ChannelManagementTitleValidation,
        QLabel#ChannelManagementTagHint[error="true"],
        QLabel#ChannelManagementCategoryHint[error="true"],
        QLabel#ChannelManagementCommercialStatus[error="true"],
        QLabel#ChannelManagementStatus[error="true"],
        QLabel#ChannelManagementCounter[invalid="true"] {
            color: %9;
        }
        QWidget#ChannelManagementSection {
            background: transparent;
            border: none;
        }
        QToolButton#ChannelManagementContentLabelsToggle {
            background: transparent;
            color: %5;
            border: 1px solid transparent;
            border-radius: %10px;
            padding: 0;
            text-align: left;
        }
        QToolButton#ChannelManagementContentLabelsToggle:hover {
            background: %2;
        }
        QToolButton#ChannelManagementContentLabelsToggle:focus {
            border-color: %16;
        }
        QPlainTextEdit#ChannelManagementTitleInput,
        QLineEdit#ChannelManagementTagInput,
        QLineEdit#ChannelManagementCategoryInput,
        QComboBox#ChannelManagementLanguageInput,
        QComboBox#ChannelManagementCommercialLength {
            background: %3;
            color: %4;
            border: 1px solid %6;
            border-radius: %10px;
            padding: %12px %11px;
        }
        QLineEdit#ChannelManagementTagInput,
        QLineEdit#ChannelManagementCategoryInput,
        QComboBox#ChannelManagementLanguageInput,
        QComboBox#ChannelManagementCommercialLength {
            min-height: %14px;
        }
        QPlainTextEdit#ChannelManagementTitleInput:focus,
        QLineEdit#ChannelManagementTagInput:focus,
        QLineEdit#ChannelManagementCategoryInput:focus,
        QComboBox#ChannelManagementLanguageInput:focus,
        QComboBox#ChannelManagementCommercialLength:focus,
        QPushButton#ChannelManagementSecondaryButton:focus,
        QPushButton#ChannelManagementCommercialButton:focus,
        QPushButton#ChannelManagementPrimaryButton:focus {
            border-color: %16;
        }
        QAbstractItemView#ChannelManagementCategoryResults,
        QComboBox#ChannelManagementLanguageInput QAbstractItemView,
        QComboBox#ChannelManagementCommercialLength QAbstractItemView {
            background: %1;
            color: %4;
            border: 1px solid %6;
            outline: none;
            selection-background-color: %7;
            selection-color: %13;
        }
        QPlainTextEdit#ChannelManagementTitleInput[invalid="true"] {
            border-color: %9;
        }
        QAbstractItemView#ChannelManagementCategoryResults::item {
            padding: %12px %11px;
        }
        QCheckBox#ChannelManagementRerunCheck,
        QWidget#ChannelManagementContentLabelRow QCheckBox {
            color: %4;
            spacing: %11px;
        }
        QCheckBox#ChannelManagementRerunCheck::indicator,
        QWidget#ChannelManagementContentLabelRow QCheckBox::indicator {
            width: %15px;
            height: %15px;
        }
        QWidget#ChannelManagementContentLabelRow[restricted="true"] QCheckBox {
            color: %5;
        }
        QLabel#ChannelManagementStatePill {
            background: transparent;
            color: %5;
            border: none;
            padding: 0;
        }
        QWidget#ChannelManagementTagEditor {
            background: %1;
            border: 1px solid %6;
            border-radius: %10px;
        }
        QWidget#ChannelManagementTagChip {
            background: %2;
            color: %4;
            border: 1px solid %6;
            border-radius: %10px;
        }
        QLabel#ChannelManagementTagText {
            background: transparent;
            color: %4;
        }
        QPushButton#ChannelManagementRemoveTag {
            background: transparent;
            color: %5;
            border: none;
            padding: 0;
        }
        QPushButton#ChannelManagementRemoveTag:hover,
        QPushButton#ChannelManagementRemoveTag:focus {
            background: %8;
            color: %9;
        }
        QPushButton#ChannelManagementSecondaryButton,
        QPushButton#ChannelManagementCommercialButton,
        QPushButton#ChannelManagementPrimaryButton {
            border: 1px solid %6;
            border-radius: %10px;
            padding: %12px %11px;
            min-height: %14px;
        }
        QPushButton#ChannelManagementSecondaryButton,
        QPushButton#ChannelManagementCommercialButton {
            background: %8;
            color: %4;
        }
        QPushButton#ChannelManagementSecondaryButton:hover,
        QPushButton#ChannelManagementCommercialButton:hover {
            background: %3;
            color: %4;
            border-color: %16;
        }
        QPushButton#ChannelManagementPrimaryButton {
            background: %7;
            color: %13;
            border-color: transparent;
        }
        QPushButton#ChannelManagementPrimaryButton:disabled {
            color: %5;
        }
        QPushButton:disabled,
        QComboBox:disabled,
        QLineEdit:disabled,
        QPlainTextEdit:disabled,
        QCheckBox:disabled {
            color: %5;
        }
        QPushButton#ChannelManagementSecondaryButton:disabled,
        QPushButton#ChannelManagementCommercialButton:disabled {
            background: %2;
            color: %5;
            border-color: %6;
        }
        QLabel#ChannelManagementCommercialConfirmation {
            color: %9;
        }
    )");
    style = style.arg(background.name())
                .arg(card.name())
                .arg(input.name())
                .arg(textColor.name())
                .arg(muted.name(QColor::HexArgb))
                .arg(border.name())
                .arg(accent.name())
                .arg(hover.name())
                .arg(danger.name())
                .arg(radius)
                .arg(paddingX)
                .arg(paddingY)
                .arg(accentText.name())
                .arg(controlMinHeight)
                .arg(std::max(14, int(14 * rawScale)))
                .arg(focusBorder.name());

    auto palette = moltorinoDialogPalette(*this->theme);
    palette.setColor(QPalette::Base, input);
    palette.setColor(QPalette::Button, card);
    palette.setColor(QPalette::PlaceholderText, muted);
    auto *paletteRoot = this->getLayoutContainer();
    paletteRoot->setAttribute(Qt::WA_WindowPropagation);
    paletteRoot->setPalette(palette);
    this->categoryPicker_->resultsView()->setPalette(palette);
    this->setStyleSheet(style);

    this->categoryPicker_->resultsView()->setStyleSheet(style);
    for (auto *widget : paletteRoot->findChildren<QWidget *>())
    {
        if (qobject_cast<QCheckBox *>(widget))
        {
            widget->ensurePolished();
            auto nativePalette = widget->palette();
            nativePalette.setColor(QPalette::Base, input);
            nativePalette.setColor(QPalette::Button, card);
            widget->setPalette(nativePalette);
        }
    }

    auto uiDemiFont = uiFont;
    uiDemiFont.setWeight(QFont::DemiBold);
    this->contentWidget_->setFont(uiFont);
    this->footerWidget_->setFont(uiFont);

    for (auto *container : {this->contentWidget_, this->footerWidget_})
    {
        for (auto *widget : container->findChildren<QWidget *>())
        {
            widget->setFont(uiFont);
        }
    }
    this->contentLabelsToggle_->setFont(uiBoldFont);
    this->categoryPicker_->resultsView()->setFont(uiFont);
    for (auto *label : this->findChildren<QLabel *>())
    {
        const auto objectName = label->objectName();
        if (objectName == QStringLiteral("ChannelManagementFieldLabel"))
        {
            label->setFont(uiBoldFont);
        }
        else if (objectName == QStringLiteral("ChannelManagementStatePill") ||
                 objectName ==
                     QStringLiteral("ChannelManagementCommercialConfirmation"))
        {
            label->setFont(uiDemiFont);
        }
    }
    for (auto *button : this->findChildren<QPushButton *>())
    {
        const auto objectName = button->objectName();
        if (objectName == QStringLiteral("ChannelManagementSecondaryButton") ||
            objectName == QStringLiteral("ChannelManagementCommercialButton") ||
            objectName == QStringLiteral("ChannelManagementPrimaryButton"))
        {
            button->setFont(uiBoldFont);
        }
    }
    this->updateTitleCounter();
    this->setStatus(this->statusText_, this->statusIsError_);
    this->setCategoryHint(this->categoryHintText_, this->categoryHintIsError_);
}

void ChannelManagementDialog::themeChangedEvent()
{
    DraggablePopup::themeChangedEvent();
    this->refreshStyle();
}

void ChannelManagementDialog::scaleChangedEvent(float scale)
{
    DraggablePopup::scaleChangedEvent(scale);
    this->refreshStyle();
}

bool ChannelManagementDialog::eventFilter(QObject *watched, QEvent *event)
{
    const auto contentChanged =
        (watched == this->contentWidget_ || watched == this->footerWidget_) &&
        event->type() == QEvent::LayoutRequest;
    const auto viewportResized = this->scrollArea_ &&
                                 watched == this->scrollArea_->viewport() &&
                                 event->type() == QEvent::Resize;
    if ((contentChanged || viewportResized) && !this->resizeQueued_)
    {
        this->resizeQueued_ = true;
        QTimer::singleShot(0, this, [this] {
            this->resizeQueued_ = false;
            this->fitToContent();
        });
    }
    return DraggablePopup::eventFilter(watched, event);
}

void ChannelManagementDialog::fitToContent()
{
    auto *screen = this->screen();
    if (!screen)
    {
        return;
    }
    const auto margin = std::max(12, int(16 * this->scale()));
    const auto available =
        screen->availableGeometry().adjusted(margin, margin, -margin, -margin);
    auto *root = this->getLayoutContainer()->layout();
    const auto margins = root->contentsMargins();
    const auto width = std::min(int(500 * this->scale()), available.width());
    const auto contentWidth =
        std::max(1, width - margins.left() - margins.right());
    if (auto *commercialRow = this->contentWidget_->findChild<QBoxLayout *>(
            "ChannelManagementCommercialRow"))
    {
        const auto contentMargins = this->contentLayout_->contentsMargins();
        const auto rowWidth =
            commercialRow->itemAt(0)->widget()->sizeHint().width() +
            commercialRow->itemAt(1)->widget()->minimumSizeHint().width() +
            commercialRow->spacing();
        commercialRow->setDirection(rowWidth > contentWidth -
                                                   contentMargins.left() -
                                                   contentMargins.right()
                                        ? QBoxLayout::TopToBottom
                                        : QBoxLayout::LeftToRight);
    }
    const auto heightForWidth = [contentWidth](QWidget *widget) {
        auto *layout = widget->layout();
        if (layout && layout->hasHeightForWidth())
        {
            return layout->totalHeightForWidth(contentWidth);
        }
        return widget->sizeHint().height();
    };
    const auto height =
        margins.top() + margins.bottom() + heightForWidth(this->headerWidget_) +
        scaledSeparatorHeight(this->scale()) + this->loadingBar_->height() +
        heightForWidth(this->contentWidget_) +
        heightForWidth(this->footerWidget_);
    this->setFixedSize(
        QSize(std::max(1, width),
              std::clamp(height, 1, std::max(1, available.height()))));

    if (this->isVisible() && !this->isMaximized() && !this->isFullScreen() &&
        !this->isMinimized())
    {
        this->move(
            std::clamp(this->x(), available.left(),
                       std::max(available.left(),
                                available.right() - this->width() + 1)),
            std::clamp(this->y(), available.top(),
                       std::max(available.top(),
                                available.bottom() - this->height() + 1)));
    }
}

void ChannelManagementDialog::windowDeactivationEvent()
{
    if (this->saveInFlight_ || this->commercialInFlight_)
    {
        return;
    }
    DraggablePopup::windowDeactivationEvent();
}

}  // namespace chatterino

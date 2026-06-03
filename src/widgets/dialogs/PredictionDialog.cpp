#include "widgets/dialogs/PredictionDialog.hpp"

#include "Application.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "util/Helpers.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/helper/Line.hpp"
#include "widgets/splits/Split.hpp"

#include <QComboBox>
#include <QCoreApplication>
#include <QCursor>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLocale>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QShowEvent>
#include <QSpinBox>
#include <QStyle>
#include <QSvgRenderer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>

namespace chatterino {

namespace {

constexpr int TITLE_LIMIT = 45;
constexpr int OUTCOME_LIMIT = 25;
constexpr int MIN_OUTCOMES = 2;
constexpr int MAX_OUTCOMES = 10;
constexpr int CREATE_VISIBLE_OUTCOME_ROWS = 4;
constexpr int MANAGE_VISIBLE_OUTCOME_ROWS = 6;
constexpr QSize DEFAULT_DIALOG_SIZE(332, 392);
constexpr int HEADER_SEPARATOR_HEIGHT = 8;
constexpr float CONTENT_SCALE_MULTIPLIER = 1.30F;
constexpr float MAX_CONTENT_SCALE_TAPER = 0.24F;
constexpr qint64 PREDICTION_TEMPLATE_CACHE_MS = 60000;

QString moltorinoAuthRequiredMessage(const QString &action)
{
    return MoltorinoAuth::authRequiredMessage(action);
}

QString normalizeMoltorinoAuthError(const QString &action, const QString &error)
{
    return MoltorinoAuth::normalizeAuthError(action, error);
}

QString normalizePredictionCreateError(const QString &error)
{
    const auto authError =
        normalizeMoltorinoAuthError("creating predictions", error);
    if (authError != error)
    {
        return authError;
    }

    const auto lowered = error.toLower();
    if (lowered.contains("forbidden") ||
        (lowered.contains("affiliate") && lowered.contains("partner")))
    {
        return "This channel is not eligible for predictions. Twitch only "
               "allows predictions on Affiliate and Partner channels.";
    }

    return error;
}

struct DurationOption {
    const char *label;
    int seconds;
};

constexpr DurationOption DURATION_OPTIONS[] = {
    {"30 seconds", 30},   {"1 minute", 60},    {"2 minutes", 120},
    {"5 minutes", 300},   {"10 minutes", 600}, {"15 minutes", 900},
    {"30 minutes", 1800},
};

const char *SVG_CHEVRON_LEFT =
    R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><path fill="currentColor" d="M15.41 7.41L14 6l-6 6 6 6 1.41-1.41L10.83 12z"/></svg>)";
const char *SVG_POINTS =
    R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><path fill="currentColor" d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm0 16c-3.31 0-6-2.69-6-6s2.69-6 6-6 6 2.69 6 6-2.69 6-6 6z"/></svg>)";
const char *SVG_TROPHY =
    R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><path fill="currentColor" d="M19 5h-2V3H7v2H5c-1.1 0-2 .9-2 2v1c0 2.55 1.92 4.63 4.39 4.94A5.01 5.01 0 0 0 11 15.9V19H7v2h10v-2h-4v-3.1a5.01 5.01 0 0 0 3.61-2.96C19.08 12.63 21 10.55 21 8V7c0-1.1-.9-2-2-2zM5 8V7h2v3.82C5.84 10.4 5 9.3 5 8zm14 0c0 1.3-.84 2.4-2 2.82V7h2v1z"/></svg>)";
const char *SVG_USERS =
    R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><path fill="currentColor" d="M16 11c1.66 0 2.99-1.34 2.99-3S17.66 5 16 5c-1.66 0-3 1.34-3 3s1.34 3 3 3zm-8 0c1.66 0 2.99-1.34 2.99-3S9.66 5 8 5C6.34 5 5 6.34 5 8s1.34 3 3 3zm0 2c-2.33 0-7 1.17-7 3.5V19h14v-2.5c0-2.33-4.67-3.5-7-3.5zm8 0c-.29 0-.62.02-.97.05 1.16.84 1.97 1.97 1.97 3.45V19h6v-2.5c0-2.33-4.67-3.5-7-3.5z"/></svg>)";
const char *SVG_CROWN =
    R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><path fill="currentColor" d="M5 16l-1-9.5 5 4L12 3l3 7.5 5-4L19 16H5zm14 2H5v2h14v-2z"/></svg>)";

QPixmap renderSvgIcon(const QString &svgData, const QColor &color, int size)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QString coloredSvg = svgData;
    coloredSvg.replace("currentColor", color.name(QColor::HexRgb));
    coloredSvg.replace("currentOpacity",
                       QString::number(color.alphaF(), 'f', 3));
    QSvgRenderer renderer(coloredSvg.toUtf8());
    if (!renderer.isValid())
    {
        return pixmap;
    }
    renderer.render(&painter);
    return pixmap;
}

/// Watches a widget for resize/layout events and calls a callback.
class ResizeWatcher : public QObject
{
public:
    explicit ResizeWatcher(QWidget *target, std::function<void()> cb,
                           QObject *parent = nullptr)
        : QObject(parent)
        , callback_(std::move(cb))
    {
        if (target)
        {
            target->installEventFilter(this);
        }
    }

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        if (ev->type() == QEvent::Resize || ev->type() == QEvent::LayoutRequest)
        {
            if (callback_)
            {
                callback_();
            }
        }
        return QObject::eventFilter(obj, ev);
    }

private:
    std::function<void()> callback_;
};

// 10 distinct, accessible colors for prediction outcomes.
// First 3 match Twitch's native Blue/Pink/Green.
constexpr std::array<const char *, 10> OUTCOME_PALETTE = {{
    "#1f8fff",  // 0  Blue
    "#e9198b",  // 1  Pink
    "#16a34a",  // 2  Green
    "#f59e0b",  // 3  Amber
    "#9147ff",  // 4  Purple
    "#14b8a6",  // 5  Teal
    "#ef4444",  // 6  Red
    "#84cc16",  // 7  Lime
    "#06b6d4",  // 8  Cyan
    "#f97316",  // 9  Orange
}};

QColor outcomeColor(int index, const QString &explicitColor = QString())
{
    // For 2-outcome predictions, honour the explicit Twitch color strings.
    if (explicitColor == "PINK")
        return QColor("#e9198b");
    if (explicitColor == "GREEN")
        return QColor("#16a34a");
    if (explicitColor == "BLUE" && index == 0)
        return QColor("#1f8fff");

    return QColor(OUTCOME_PALETTE[index % OUTCOME_PALETTE.size()]);
}

QString formatStatus(const QString &status)
{
    if (status == "ACTIVE")
    {
        return "Active";
    }
    if (status == "LOCKED")
    {
        return "Locked";
    }
    if (status == "RESOLVED")
    {
        return "Resolved";
    }
    if (status == "CANCELED")
    {
        return "Canceled";
    }
    return status;
}

QColor statusColor(const QString &status)
{
    if (status == "ACTIVE")
    {
        return QColor("#9147ff");
    }
    if (status == "LOCKED")
    {
        return QColor("#d99024");
    }
    if (status == "RESOLVED")
    {
        return QColor("#22c55e");
    }
    if (status == "CANCELED")
    {
        return QColor("#9ca3af");
    }
    return QColor("#64748b");
}

QString formatCompact(qlonglong value)
{
    return formatCompactNumber(value);
}

QString formatRemainingTime(int totalSeconds)
{
    const int clamped = std::max(0, totalSeconds);
    const int minutes = clamped / 60;
    const int seconds = clamped % 60;
    return QString("%1:%2").arg(minutes).arg(seconds, 2, 10, QChar('0'));
}

bool hasOpenPrediction(
    const std::optional<TwitchChannel::PredictionEvent> &prediction)
{
    return prediction.has_value() &&
           (prediction->status == "ACTIVE" || prediction->status == "LOCKED");
}

const TwitchChannel::PredictionOutcome *findPredictionOutcome(
    const TwitchChannel::PredictionEvent &prediction, const QString &outcomeId)
{
    for (const auto &outcome : prediction.outcomes)
    {
        if (outcome.id == outcomeId)
        {
            return &outcome;
        }
    }

    return nullptr;
}

float contentScale(float scale)
{
    const float taper = std::clamp((scale - 1.0F) / 0.6F, 0.0F, 1.0F);
    const float multiplier =
        CONTENT_SCALE_MULTIPLIER - taper * MAX_CONTENT_SCALE_TAPER;
    return scale * multiplier;
}

int scaledSeparatorHeight(float scale)
{
    return std::max(1, int(HEADER_SEPARATOR_HEIGHT * scale));
}

}  // namespace

class PredictionTemplatePicker : public QWidget
{
public:
    explicit PredictionTemplatePicker(QWidget *parent = nullptr)
        : QWidget(parent)
        , input_(new QLineEdit(this))
        , button_(new QToolButton(this))
    {
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        this->setObjectName("PredictionTitlePicker");
        this->input_->setObjectName("PredictionTitleInput");
        this->button_->setObjectName("PredictionTitlePickerButton");
        this->button_->setArrowType(Qt::DownArrow);
        this->button_->setCursor(Qt::PointingHandCursor);
        this->button_->setFocusPolicy(Qt::NoFocus);
        this->setFocusProxy(this->input_);

        layout->addWidget(this->input_, 1);
        layout->addWidget(this->button_, 0);

        QObject::connect(this->button_, &QToolButton::clicked, this, [this] {
            this->requestPopup();
        });
        QObject::connect(this->input_, &QLineEdit::returnPressed, this, [this] {
            this->closePopup();
        });
    }

    ~PredictionTemplatePicker() override
    {
        this->closePopup();
    }

    QLineEdit *lineEdit() const
    {
        return this->input_;
    }

    QString text() const
    {
        return this->input_->text();
    }

    void setText(const QString &text)
    {
        this->input_->setText(text);
    }

    void setFont(const QFont &font)
    {
        QWidget::setFont(font);
        this->input_->setFont(font);
        this->button_->setFont(font);
    }

    void setFixedHeight(int height)
    {
        QWidget::setFixedHeight(height);
        this->input_->setFixedHeight(height);
        this->button_->setFixedSize(height, height);
    }

    void setItems(const QStringList &labels)
    {
        this->items_ = labels;
        this->statusText_.clear();
        this->setProperty("PredictionTemplatesPopulated", true);
        this->populatePopup();
    }

    void setStatusText(const QString &text)
    {
        this->items_.clear();
        this->statusText_ = text;
        this->setProperty("PredictionTemplatesPopulated", true);
        this->populatePopup();
    }

    void requestPopup()
    {
        if (this->popup_ != nullptr)
        {
            this->closePopup();
            return;
        }

        if (this->beforeShowPopup && !this->beforeShowPopup())
        {
            return;
        }
        this->showPopup();
    }

    void showPopup()
    {
        this->closePopup();

        auto *popup = new QListWidget(nullptr);
        popup->setObjectName("PredictionTitleTemplatePopup");
        popup->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint |
                              Qt::NoDropShadowWindowHint);
        popup->setAttribute(Qt::WA_DeleteOnClose);
        popup->setAttribute(Qt::WA_ShowWithoutActivating);
        popup->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        popup->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        popup->setUniformItemSizes(true);
        popup->setFont(this->input_->font());
        popup->setMouseTracking(true);
        if (auto *topLevel = this->window())
        {
            popup->setStyleSheet(topLevel->styleSheet());
        }

        QObject::connect(popup, &QListWidget::itemClicked, this,
                         [this](QListWidgetItem *item) {
                             bool ok = false;
                             const int index =
                                 item->data(Qt::UserRole).toInt(&ok);
                             auto activated = this->activated;
                             this->closePopup();
                             if (ok && activated)
                             {
                                 activated(index);
                             }
                         });
        QObject::connect(popup, &QListWidget::itemEntered, popup,
                         [popup](QListWidgetItem *item) {
                             if ((item->flags() & Qt::ItemIsEnabled) != 0)
                             {
                                 popup->setCurrentItem(item);
                             }
                         });
        QObject::connect(popup, &QObject::destroyed, this, [this, popup] {
            if (this->popup_ == popup)
            {
                QCoreApplication::instance()->removeEventFilter(this);
                this->popup_ = nullptr;
            }
        });

        this->popup_ = popup;
        QCoreApplication::instance()->installEventFilter(this);
        this->populatePopup();
        popup->show();
    }

    void closePopup()
    {
        if (this->popup_ != nullptr)
        {
            QCoreApplication::instance()->removeEventFilter(this);
            this->popup_->close();
            this->popup_ = nullptr;
        }
    }

    std::function<bool()> beforeShowPopup;
    std::function<void(int)> activated;

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (this->popup_ == nullptr)
        {
            return QWidget::eventFilter(object, event);
        }

        if (event->type() == QEvent::ApplicationDeactivate)
        {
            this->closePopup();
            return QWidget::eventFilter(object, event);
        }

        if (event->type() != QEvent::MouseButtonPress &&
            event->type() != QEvent::Wheel)
        {
            return QWidget::eventFilter(object, event);
        }

        auto *widget = qobject_cast<QWidget *>(object);
        if (widget == nullptr)
        {
            return QWidget::eventFilter(object, event);
        }

        if (widget == this->popup_ || this->popup_->isAncestorOf(widget) ||
            widget == this || this->isAncestorOf(widget))
        {
            return QWidget::eventFilter(object, event);
        }

        this->closePopup();
        return QWidget::eventFilter(object, event);
    }

private:
    void populatePopup()
    {
        if (this->popup_ == nullptr)
        {
            return;
        }

        this->popup_->clear();
        if (!this->statusText_.isEmpty())
        {
            auto *item = new QListWidgetItem(this->statusText_, this->popup_);
            item->setFlags(Qt::NoItemFlags);
        }
        else
        {
            for (int i = 0; i < this->items_.size(); ++i)
            {
                auto *item =
                    new QListWidgetItem(this->items_.at(i), this->popup_);
                item->setData(Qt::UserRole, i);
            }
        }

        this->resizePopup();
    }

    void resizePopup()
    {
        if (this->popup_ == nullptr)
        {
            return;
        }

        const int itemCount = std::max(1, this->popup_->count());
        int rowHeight = this->popup_->sizeHintForRow(0);
        if (rowHeight <= 0)
        {
            rowHeight = this->fontMetrics().height() + 4;
        }
        const int frame = this->popup_->frameWidth() * 2;
        this->popup_->setFixedSize(this->width(),
                                   rowHeight * itemCount + frame);
        this->popup_->move(this->mapToGlobal(QPoint(0, this->height())));
    }

    QLineEdit *input_{};
    QToolButton *button_{};
    QStringList items_;
    QString statusText_;
    QPointer<QListWidget> popup_;
};

std::vector<QPointer<PredictionDialog>> PredictionDialog::activeDialogs_;

PredictionDialog::PredictionDialog(TwitchChannel *channel, QWidget *parent)
    : DraggablePopup(true, parent)
    , channel_(channel)
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setObjectName("PredictionDialog");
    this->setWindowTitle("Prediction");
    this->setScaleIndependentSize(DEFAULT_DIALOG_SIZE);

    this->ensureCreateDraft();
    this->currentPrediction_ = *this->channel_->accessPrediction();
    this->managedConnections_.emplace_back(
        this->channel_->predictionChanged.connect([this] {
            this->setPrediction(*this->channel_->accessPrediction());
        }));

    auto *container = this->getLayoutContainer();
    container->setObjectName("PredictionDialogRoot");
    container->setMouseTracking(true);

    this->mainLayout_ = new QVBoxLayout(container);

    this->headerWidget_ = new QWidget(container);

    auto *headerLayout = new QHBoxLayout(this->headerWidget_);
    auto *headerTextLayout = new QVBoxLayout();
    headerTextLayout->setContentsMargins(0, 0, 0, 0);

    this->headerTitleLabel_ = new QLabel(this->headerWidget_);
    this->headerTitleLabel_->setObjectName("PredictionHeaderTitle");
    headerTextLayout->addWidget(this->headerTitleLabel_);

    this->headerSubtitleLabel_ = new QLabel(this->headerWidget_);
    this->headerSubtitleLabel_->setObjectName("PredictionHeaderSubtitle");
    headerTextLayout->addWidget(this->headerSubtitleLabel_);

    headerLayout->addLayout(headerTextLayout);
    headerLayout->addStretch(1);

    this->modBettingView_ = (getSettings()->predictionModAction == 0);

    this->modToggleButton_ = new QPushButton(this->headerWidget_);
    this->modToggleButton_->setObjectName("PredictionModToggleButton");
    this->modToggleButton_->setCursor(Qt::PointingHandCursor);
    this->modToggleButton_->hide();
    QObject::connect(this->modToggleButton_, &QPushButton::clicked, this,
                     [this] {
                         this->modBettingView_ = !this->modBettingView_;
                         this->updateUI();
                     });
    headerLayout->addWidget(this->modToggleButton_);

    this->pinButton_ = this->createPinButton();
    this->pinButton_->setToolTip("Pin Prediction Popup");
    this->pinButton_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    if (!getSettings()->predictionCloseOnFocusLoss)
    {
        this->ensurePinned();
    }
    headerLayout->addWidget(this->pinButton_);

    this->closeButton_ = new SvgButton(
        {
            .dark = ":/buttons/cancel.svg",
            .light = ":/buttons/cancelDark.svg",
        },
        this, QSize(3, 3));
    this->closeButton_->setScaleIndependentSize(18, 18);
    this->closeButton_->setToolTip("Close");
    this->closeButton_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    QObject::connect(this->closeButton_, &Button::leftClicked, this, [this] {
        this->close();
    });
    headerLayout->addWidget(this->closeButton_);

    this->mainLayout_->addWidget(this->headerWidget_);
    auto *headerSeparator = new Line(false);
    headerSeparator->setObjectName("PredictionDialogSeparator");
    headerSeparator->setFixedHeight(scaledSeparatorHeight(this->scale()));
    this->mainLayout_->addWidget(headerSeparator);

    this->scrollArea_ = new QScrollArea(container);
    this->scrollArea_->setObjectName("PredictionScrollArea");
    this->scrollArea_->setFrameShape(QFrame::NoFrame);
    this->scrollArea_->setWidgetResizable(true);
    this->scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->scrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    this->scrollArea_->viewport()->installEventFilter(this);
    this->mainLayout_->addWidget(this->scrollArea_);

    this->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);

    this->barsAnim_ = new QVariantAnimation(this);
    this->barsAnim_->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(
        this->barsAnim_, &QVariantAnimation::valueChanged, this,
        [this](const QVariant &value) {
            double progress = value.toDouble();
            for (auto it = this->targetBarWidths_.constBegin();
                 it != this->targetBarWidths_.constEnd(); ++it)
            {
                const QString &outcomeId = it.key();
                int targetWidth = it.value();
                int startWidth = this->previousBarWidths_.contains(outcomeId)
                                     ? this->previousBarWidths_[outcomeId]
                                     : targetWidth;
                int newWidth =
                    startWidth +
                    static_cast<int>((targetWidth - startWidth) * progress);
                auto *bar =
                    this->findChild<QWidget *>("PredictionBar_" + outcomeId);
                if (bar)
                {
                    bar->setFixedWidth(newWidth);
                }
            }
        });

    this->updateUI();
}

void PredictionDialog::showDialog(
    TwitchChannel *channel, QWidget *parent,
    const std::optional<TwitchChannel::PredictionEvent> &prediction)
{
    auto &s = *getSettings();
    if (s.limitPredictionDialogs)
    {
        // Clean up and check current dialogs
        auto it = activeDialogs_.begin();
        while (it != activeDialogs_.end())
        {
            if (it->isNull())
            {
                it = activeDialogs_.erase(it);
                continue;
            }

            if (s.predictionDialogsPerChannel)
            {
                if ((*it)->channel_ == channel)
                {
                    if (prediction.has_value())
                    {
                        (*it)->setPrediction(prediction);
                    }
                    (*it)->raise();
                    (*it)->activateWindow();
                    return;
                }
            }
            else
            {
                // Global limit: close the old one
                (*it)->close();
                it = activeDialogs_.erase(it);
                continue;
            }
            ++it;
        }
    }

    auto *dialog = new PredictionDialog(channel, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    activeDialogs_.push_back(dialog);

    if (prediction.has_value())
    {
        dialog->setPrediction(prediction);
    }

    // Initial positioning
    QPoint center = QCursor::pos();
    if (auto *split = dynamic_cast<Split *>(parent))
    {
        if (auto *topLevel = split->window())
        {
            center = topLevel->geometry().center();
        }
    }
    else if (parent && parent->window())
    {
        center = parent->window()->geometry().center();
    }

    dialog->show();
    const auto sz = dialog->size();
    dialog->showAndMoveTo(center - QPoint(sz.width() / 2, sz.height() / 2),
                          widgets::BoundsChecking::DesiredPosition);
    dialog->raise();
    dialog->activateWindow();
}

void PredictionDialog::setPrediction(
    const std::optional<TwitchChannel::PredictionEvent> &prediction)
{
    const bool broadcasterView = this->isBroadcasterView();
    bool canUpdateInPlace =
        this->currentPrediction_.has_value() && prediction.has_value() &&
        this->currentPrediction_->id == prediction->id &&
        this->currentPrediction_->status == prediction->status &&
        this->currentPrediction_->outcomes.size() ==
            prediction->outcomes.size();

    this->currentPrediction_ = prediction;
    if (broadcasterView || !this->currentPrediction_.has_value() ||
        this->currentPrediction_->outcomes.size() <= 2)
    {
        this->selectedBettingOutcomeId_.clear();
    }
    else if (!this->selectedBettingOutcomeId_.isEmpty() &&
             findPredictionOutcome(*this->currentPrediction_,
                                   this->selectedBettingOutcomeId_) == nullptr)
    {
        this->selectedBettingOutcomeId_.clear();
    }

    if (canUpdateInPlace && this->activeWidget_ != nullptr &&
        this->renderedBroadcasterView_ == broadcasterView)
    {
        this->updateInPlace();
        return;
    }

    this->updateUI();
}

void PredictionDialog::updateInPlace()
{
    if (!this->currentPrediction_.has_value())
    {
        return;
    }

    const auto &prediction = *this->currentPrediction_;
    const float rawScale = this->scale();
    const float effectiveScale = contentScale(rawScale);
    const auto locale = QLocale();

    qlonglong totalPoints = 0;
    for (const auto &outcome : prediction.outcomes)
    {
        totalPoints += outcome.totalPoints;
    }

    // Snapshot old bar widths before computing new targets
    QHash<QString, int> newTargets;

    for (int i = 0; i < static_cast<int>(prediction.outcomes.size()); ++i)
    {
        const auto &outcome = prediction.outcomes.at(i);
        const int pct =
            totalPoints > 0
                ? static_cast<int>((outcome.totalPoints * 100.0) / totalPoints)
                : 0;
        const double multiplier =
            outcome.totalPoints > 0
                ? static_cast<double>(totalPoints) / outcome.totalPoints
                : 0.0;

        // Update percentage labels (both 2-outcome and multi-outcome views)
        auto *pctLabel =
            this->findChild<QLabel *>("PredictionPct_" + outcome.id);
        if (pctLabel)
        {
            pctLabel->setText(QString::number(pct) + "%");
        }

        // Update return/multiplier labels (multi-outcome list view)
        auto *returnLabel =
            this->findChild<QLabel *>("PredictionReturn_" + outcome.id);
        if (returnLabel)
        {
            returnLabel->setText(QString::number(multiplier, 'f', 1) + "x");
        }

        // Update stats labels (manage view)
        auto *statsLabel =
            this->findChild<QLabel *>("PredictionStats_" + outcome.id);
        if (statsLabel)
        {
            statsLabel->setText(
                QString("%1 pts · %2 users · %3")
                    .arg(formatChannelPoints(outcome.totalPoints))
                    .arg(locale.toString(outcome.totalUsers))
                    .arg(QString::number(pct) + "%"));
        }

        // Update stat value labels in the 2-outcome betting view.
        auto statWidgets =
            this->findChildren<QLabel *>("PredictionBetStatValue");
        for (auto *w : statWidgets)
        {
            if (w->property("outcomeId").toString() != outcome.id)
            {
                continue;
            }

            const auto statRole = w->property("statRole").toString();
            if (statRole == "points")
            {
                w->setText(formatCompact(outcome.totalPoints));
            }
            else if (statRole == "multiplier")
            {
                w->setText(QString::number(multiplier, 'f', 1) + "x");
            }
            else if (statRole == "users")
            {
                w->setText(locale.toString(outcome.totalUsers));
            }
            else if (statRole == "crown")
            {
                w->setText(formatCompact(outcome.topPoints));
                w->setToolTip(outcome.topPredictorName);
            }
        }

        auto detailStatWidgets =
            this->findChildren<QLabel *>("PredictionBetDetailValue");
        for (auto *w : detailStatWidgets)
        {
            if (w->property("outcomeId").toString() != outcome.id)
            {
                continue;
            }

            const auto statRole = w->property("statRole").toString();
            if (statRole == "points")
            {
                w->setText(formatCompact(outcome.totalPoints));
            }
            else if (statRole == "multiplier")
            {
                w->setText(QString::number(multiplier, 'f', 1) + "x");
            }
            else if (statRole == "users")
            {
                w->setText(locale.toString(outcome.totalUsers));
            }
            else if (statRole == "crown")
            {
                w->setText(formatCompact(outcome.topPoints));
                w->setToolTip(outcome.topPredictorName);
            }
        }

        // Calculate and store target bar width for animation
        int fillW =
            std::max(16, int(80 * (std::max(1, pct) / 100.0) * effectiveScale));
        newTargets[outcome.id] = fillW;
    }

    // Snapshot current bar widths as animation start points
    if (this->barsAnim_->state() == QAbstractAnimation::Running)
    {
        // If already animating, capture current interpolated widths
        double progress = this->barsAnim_->currentValue().toDouble();
        for (auto it = this->targetBarWidths_.constBegin();
             it != this->targetBarWidths_.constEnd(); ++it)
        {
            int startW = this->previousBarWidths_.contains(it.key())
                             ? this->previousBarWidths_[it.key()]
                             : it.value();
            this->previousBarWidths_[it.key()] =
                startW + static_cast<int>((it.value() - startW) * progress);
        }
    }
    else
    {
        // Capture actual current widths from widgets
        for (auto it = newTargets.constBegin(); it != newTargets.constEnd();
             ++it)
        {
            auto *bar = this->findChild<QWidget *>("PredictionBar_" + it.key());
            if (bar)
            {
                this->previousBarWidths_[it.key()] = bar->width();
            }
        }
    }

    this->targetBarWidths_ = newTargets;
    this->barsAnim_->stop();
    this->barsAnim_->setDuration(400);
    this->barsAnim_->setStartValue(0.0);
    this->barsAnim_->setEndValue(1.0);
    this->barsAnim_->start();

    // Refresh the header subtitle (timer, status text)
    this->refreshHeader();
}

void PredictionDialog::themeChangedEvent()
{
    DraggablePopup::themeChangedEvent();
    this->refreshStyle();
}

void PredictionDialog::scaleChangedEvent(float scale)
{
    DraggablePopup::scaleChangedEvent(scale);

    this->updateUI();
}

void PredictionDialog::showEvent(QShowEvent *event)
{
    DraggablePopup::showEvent(event);

    QTimer::singleShot(0, this, [this] {
        this->applySizeConstraints(false);
        this->settleLayoutAfterResize();
    });
}

void PredictionDialog::resizeEvent(QResizeEvent *event)
{
    DraggablePopup::resizeEvent(event);

    const int layoutGeneration = this->layoutUpdateGeneration_;
    QTimer::singleShot(0, this, [this, layoutGeneration] {
        if (layoutGeneration != this->layoutUpdateGeneration_)
        {
            return;
        }

        this->settleLayoutAfterResize();
    });
}

bool PredictionDialog::eventFilter(QObject *watched, QEvent *event)
{
    // Block wheel events on the outer scroll area when scrolling is disabled.
    if (watched == this->scrollArea_->viewport() &&
        event->type() == QEvent::Wheel)
    {
        return true;
    }

    if (event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseButtonRelease ||
        event->type() == QEvent::MouseButtonDblClick)
    {
        const auto outcomeId =
            watched->property("predictionBetOutcomeId").toString();
        if (!outcomeId.isEmpty() && !this->isBroadcasterView())
        {
            this->selectedBettingOutcomeId_ = outcomeId;
            this->updateUI();
            return true;
        }
    }

    if ((event->type() == QEvent::MouseButtonPress ||
         event->type() == QEvent::MouseButtonRelease) &&
        this->manageResolveCombo_ != nullptr)
    {
        const auto outcomeId =
            watched->property("predictionOutcomeId").toString();
        if (!outcomeId.isEmpty())
        {
            const int index = this->manageResolveCombo_->findData(outcomeId);
            if (index >= 0)
            {
                this->selectedManageOutcomeId_ = outcomeId;
                this->manageResolveCombo_->setCurrentIndex(index);
                this->updateManageOutcomeSelection();
                if (event->type() == QEvent::MouseButtonRelease)
                {
                    return true;
                }
            }
        }
    }

    return DraggablePopup::eventFilter(watched, event);
}

void PredictionDialog::updateManageOutcomeSelection()
{
    const auto selectedId =
        this->manageResolveCombo_ != nullptr
            ? this->manageResolveCombo_->currentData().toString()
            : this->selectedManageOutcomeId_;

    for (auto *card : this->findChildren<QWidget *>("PredictionOutcomeRowCard"))
    {
        const bool selected =
            !selectedId.isEmpty() &&
            card->property("predictionOutcomeId").toString() == selectedId;
        if (card->property("selectedManageOutcome").toBool() == selected)
        {
            continue;
        }

        card->setProperty("selectedManageOutcome", selected);
        card->style()->unpolish(card);
        card->style()->polish(card);
        card->update();
    }
}

void PredictionDialog::settleLayoutAfterResize()
{
    if (auto *container = this->getLayoutContainer())
    {
        container->updateGeometry();
        if (auto *layout = container->layout())
        {
            layout->invalidate();
            layout->activate();
        }
    }

    if (this->mainLayout_ != nullptr)
    {
        this->mainLayout_->invalidate();
        this->mainLayout_->activate();
    }

    if (this->activeWidget_ != nullptr)
    {
        this->activeWidget_->updateGeometry();
        if (auto *layout = this->activeWidget_->layout())
        {
            layout->invalidate();
            layout->activate();
        }
    }

    this->fitBettingOutcomesList();

    if (this->scrollArea_ != nullptr)
    {
        this->scrollArea_->updateGeometry();
    }
    if (this->outcomesScrollArea_ != nullptr)
    {
        this->outcomesScrollArea_->updateGeometry();
    }
}

void PredictionDialog::fitBettingOutcomesList()
{
    if (!this->currentPrediction_ ||
        this->currentPrediction_->outcomes.size() <= 2 ||
        !this->selectedBettingOutcomeId_.isEmpty() ||
        this->scrollArea_ == nullptr || this->outcomesScrollArea_ == nullptr ||
        this->bettingOutcomesPanel_ == nullptr)
    {
        return;
    }

    const int wanted =
        this->bettingOutcomesPanel_->property("predictionWantedHeight").toInt();
    if (wanted <= 0)
    {
        return;
    }

    this->bettingOutcomesPanel_->setMinimumHeight(wanted);
    this->bettingOutcomesPanel_->setMaximumHeight(wanted);
    this->outcomesScrollArea_->setMinimumHeight(0);
    this->outcomesScrollArea_->setMaximumHeight(QWIDGETSIZE_MAX);
}

void PredictionDialog::ensureCreateDraft()
{
    while (this->draftOutcomes_.size() < MIN_OUTCOMES)
    {
        this->draftOutcomes_.append(QString());
    }

    while (this->draftOutcomes_.size() > MAX_OUTCOMES)
    {
        this->draftOutcomes_.removeLast();
    }

    this->draftDurationSeconds_ =
        std::clamp(this->draftDurationSeconds_, 30, 1800);
}

void PredictionDialog::applySizeConstraints(bool preserveCurrentPosition)
{
    auto *screen = this->screen();
    if (screen == nullptr)
    {
        screen = QGuiApplication::screenAt(QCursor::pos());
    }
    if (screen == nullptr)
    {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr)
    {
        return;
    }

    const auto available = screen->availableGeometry();
    const int screenMargin = std::max(12, int(16 * this->scale()));
    const int maxWidth = std::max(1, available.width() - screenMargin * 2);
    const int maxHeight = std::max(1, available.height() - screenMargin * 2);
    // All views with an active prediction use content-driven height.
    // Create mode (no prediction) uses the fixed scale-independent size.
    const bool createMode = !hasOpenPrediction(this->currentPrediction_) &&
                            this->channel_->hasModRights();
    const bool useContentDrivenHeight =
        this->currentPrediction_.has_value() && !createMode;
    const int targetWidth =
        std::min(int(this->scaleIndependentWidth() * this->scale()), maxWidth);
    int targetHeight = std::min(
        int(this->scaleIndependentHeight() * this->scale()), maxHeight);

    if (useContentDrivenHeight)
    {
        auto *container = this->getLayoutContainer();
        if (container != nullptr)
        {
            container->ensurePolished();
            if (auto *layout = container->layout())
            {
                layout->invalidate();
                layout->activate();
            }
        }

        if (this->headerWidget_ != nullptr)
        {
            this->headerWidget_->ensurePolished();
        }

        if (this->activeWidget_ != nullptr)
        {
            this->activeWidget_->ensurePolished();
            if (auto *layout = this->activeWidget_->layout())
            {
                layout->invalidate();
                layout->activate();
            }
        }

        int contentHeight = 0;
        QMargins mainMargins;
        int contentWidth = targetWidth;
        if (this->mainLayout_ != nullptr)
        {
            mainMargins = this->mainLayout_->contentsMargins();
            contentHeight += mainMargins.top() + mainMargins.bottom();
            contentWidth -= mainMargins.left() + mainMargins.right();
        }
        contentWidth = std::max(1, contentWidth - 2);

        if (this->headerWidget_ != nullptr)
        {
            int headerHeight = this->headerWidget_->sizeHint().height();
            if (auto *layout = this->headerWidget_->layout();
                layout != nullptr && layout->hasHeightForWidth())
            {
                headerHeight = std::max(
                    headerHeight, layout->totalHeightForWidth(contentWidth));
            }
            contentHeight += headerHeight;
        }
        contentHeight += scaledSeparatorHeight(this->scale());
        if (this->activeWidget_ != nullptr)
        {
            int activeHeight = 0;
            if (auto *layout = this->activeWidget_->layout())
            {
                if (layout->hasHeightForWidth())
                {
                    activeHeight = layout->totalHeightForWidth(contentWidth);
                }
                if (activeHeight == 0)
                {
                    activeHeight = layout->sizeHint().height();
                }
            }
            if (activeHeight == 0)
            {
                activeHeight = this->activeWidget_->sizeHint().height();
            }
            activeHeight += std::max(2, int(3 * this->scale()));
            contentHeight += activeHeight;
        }

        if (this->bottomWidget_ != nullptr &&
            this->bottomWidget_->isVisibleTo(this))
        {
            int bottomHeight = 0;
            if (auto *layout = this->bottomWidget_->layout())
            {
                if (layout->hasHeightForWidth())
                {
                    bottomHeight = layout->totalHeightForWidth(contentWidth);
                }
                if (bottomHeight == 0)
                {
                    bottomHeight = layout->sizeHint().height();
                }
            }
            if (bottomHeight == 0)
            {
                bottomHeight = this->bottomWidget_->sizeHint().height();
            }
            contentHeight += bottomHeight;
        }

        if (container != nullptr)
        {
            // Do NOT use container->minimumSizeHint().height().
            // For word-wrapped labels, minimumSizeHint() calculates the height required
            // if the text were compressed to the width of the longest single word.
            // This wildly inflates the required height and causes massive vertical padding.
            // totalHeightForWidth(contentWidth) is the compact target height.
            // Add a tiny safety buffer above instead of clamping to sizeHint(),
            // because sizeHint() can over-reserve after rapid zoom changes.
        }

        contentHeight += std::max(4, int(6 * this->scale()));

        if (contentHeight > 0)
        {
            targetHeight = std::min(contentHeight, maxHeight);
        }
    }

    const QPoint currentPosition = this->pos();
    this->setFixedSize(targetWidth, targetHeight);
    if (auto *layout = this->layout())
    {
        layout->invalidate();
        layout->activate();
    }
    if (this->scrollArea_ != nullptr)
    {
        this->scrollArea_->updateGeometry();
        if (auto *widget = this->scrollArea_->widget())
        {
            widget->updateGeometry();
        }
    }
    if (this->outcomesScrollArea_ != nullptr)
    {
        this->outcomesScrollArea_->updateGeometry();
        if (auto *widget = this->outcomesScrollArea_->widget())
        {
            widget->updateGeometry();
        }
    }

    if (this->isVisible())
    {
        if (preserveCurrentPosition)
        {
            this->moveTo(currentPosition,
                         widgets::BoundsChecking::DesiredPosition);
        }
        else
        {
            this->applyLastBoundsCheck();
        }
    }
}

void PredictionDialog::refreshHeader()
{
    if (!hasOpenPrediction(this->currentPrediction_) &&
        this->channel_->hasModRights())
    {
        this->headerTitleLabel_->setText("Start a Prediction");
        this->headerSubtitleLabel_->setText(
            QString("#%1").arg(this->channel_->getName()));
        this->modToggleButton_->hide();
        return;
    }

    if (this->currentPrediction_)
    {
        this->headerTitleLabel_->setText("Prediction");
        this->headerSubtitleLabel_->setText(
            QString("<span style=\"font-weight:400;\">#%1</span>"
                    " <span style=\"font-weight:400;\">•</span> "
                    "<span style=\"font-weight:700; color:%2;\">%3</span>")
                .arg(this->channel_->getName().toHtmlEscaped(),
                     statusColor(this->currentPrediction_->status).name(),
                     formatStatus(this->currentPrediction_->status)
                         .toLower()
                         .toHtmlEscaped()));

        if (hasOpenPrediction(this->currentPrediction_) &&
            this->channel_->hasModRights())
        {
            const bool broadcasterView = this->isBroadcasterView();
            if (!broadcasterView && this->currentPrediction_->selfPoints > 0)
            {
                this->modToggleButton_->hide();
            }
            else
            {
                this->modToggleButton_->setText(
                    this->modBettingView_
                        ? "Manage"
                        : (broadcasterView ? "Overview" : "Bet"));
                this->modToggleButton_->setToolTip(
                    this->modBettingView_
                        ? "Switch to manage view (lock/resolve/cancel)"
                        : (broadcasterView ? "Switch to prediction overview"
                                           : "Switch to betting view"));
                this->modToggleButton_->show();
            }
        }
        else
        {
            this->modToggleButton_->hide();
        }
        return;
    }

    this->headerTitleLabel_->setText("Prediction");
    this->headerSubtitleLabel_->setText(
        QString("#%1 • no active prediction").arg(this->channel_->getName()));
    this->modToggleButton_->hide();
}

void PredictionDialog::updateUI()
{
    // Suppress repaints during the full widget rebuild cycle.
    // The guard counter ensures rapid-fire calls (e.g. during zoom)
    // won't re-enable painting before the last rebuild settles.
    this->updateGuard_++;
    const int layoutGeneration = ++this->layoutUpdateGeneration_;
    this->setUpdatesEnabled(false);

    this->refreshHeader();

    const bool broadcasterView = this->isBroadcasterView();
    this->renderedBroadcasterView_ = broadcasterView;

    if (broadcasterView || !this->currentPrediction_.has_value() ||
        this->currentPrediction_->outcomes.size() <= 2)
    {
        this->selectedBettingOutcomeId_.clear();
    }
    else if (!this->selectedBettingOutcomeId_.isEmpty() &&
             findPredictionOutcome(*this->currentPrediction_,
                                   this->selectedBettingOutcomeId_) == nullptr)
    {
        this->selectedBettingOutcomeId_.clear();
    }

    const float rawScale = this->scale();
    const float effectiveScale = contentScale(rawScale);

    this->mainScrollValue_ = this->scrollArea_->verticalScrollBar()->value();

    this->outcomesScrollArea_ = nullptr;
    this->bettingOutcomesPanel_ = nullptr;
    this->manageResolveCombo_ = nullptr;

    if (this->bottomWidget_)
    {
        this->bottomWidget_->deleteLater();
    }
    this->bottomWidget_ = nullptr;

    if (auto *oldWidget = this->scrollArea_->takeWidget())
    {
        oldWidget->deleteLater();
    }
    this->activeWidget_ = new QWidget();
    this->activeWidget_->setObjectName("PredictionDialogContent");
    this->activeWidget_->setFont(
        getApp()->getFonts()->getFont(FontStyle::UiMedium, effectiveScale));
    this->activeWidget_->setMinimumWidth(0);
    this->activeWidget_->setSizePolicy(QSizePolicy::Ignored,
                                       QSizePolicy::Preferred);

    const int spacing = std::max(2, int(3 * rawScale));
    const int margin = std::max(3, int(5 * rawScale));

    auto *layout = new QVBoxLayout(this->activeWidget_);
    const bool createMode = !hasOpenPrediction(this->currentPrediction_) &&
                            this->channel_->hasModRights();
    const int topMargin = margin;
    const int bottomMargin = this->currentPrediction_.has_value()
                                 ? margin
                                 : std::max(6, int(10 * rawScale));

    layout->setContentsMargins(margin, topMargin, margin, bottomMargin);
    layout->setSpacing(spacing);

    if (!hasOpenPrediction(this->currentPrediction_))
    {
        if (this->channel_->hasModRights())
        {
            this->buildCreateUI();
        }
        else
        {
            auto *card = new QWidget(this->activeWidget_);
            card->setObjectName("PredictionCard");
            auto *cardLayout = new QVBoxLayout(card);
            auto *label =
                new QLabel("No active prediction in this channel.", card);
            label->setObjectName("PredictionInfoLabel");
            label->setWordWrap(true);
            label->setAlignment(Qt::AlignCenter);
            cardLayout->addWidget(label);
            layout->addWidget(card);
        }
    }
    else if (this->channel_->hasModRights())
    {
        if (!broadcasterView && this->currentPrediction_ &&
            this->currentPrediction_->selfPoints > 0)
        {
            this->modBettingView_ = true;
            this->buildBettingUI();
        }
        else if (this->modBettingView_)
        {
            this->buildBettingUI();
        }
        else
        {
            this->buildManageUI();
        }
    }
    else
    {
        this->buildBettingUI();
    }

    if (!createMode && !this->currentPrediction_)
    {
        layout->addStretch(1);
    }

    // All views: disable outer scrollbar. Manage view caps its outcomes
    // in a fixed-height inner scroll area; betting/create are content-driven.
    this->scrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->scrollArea_->setWidget(this->activeWidget_);
    QTimer::singleShot(0, this, [this, layoutGeneration] {
        if (layoutGeneration != this->layoutUpdateGeneration_)
        {
            return;
        }

        this->scrollArea_->verticalScrollBar()->setValue(
            this->mainScrollValue_);
    });
    this->refreshStyle();
    this->applySizeConstraints(true);

    // Deferred pass: after Qt processes the stylesheet / font changes and
    // after BaseWidget::setScale's setScaleIndependentSize (which may
    // overwrite the content-driven height), recompute the correct size
    // and then show the result in a single repaint.
    QTimer::singleShot(0, this, [this, layoutGeneration] {
        if (layoutGeneration != this->layoutUpdateGeneration_)
        {
            return;
        }

        this->applySizeConstraints(true);
        this->settleLayoutAfterResize();
        this->updateGuard_ = 0;
        this->setUpdatesEnabled(true);
    });
    QTimer::singleShot(16, this, [this, layoutGeneration] {
        if (layoutGeneration != this->layoutUpdateGeneration_)
        {
            return;
        }

        this->applySizeConstraints(true);
        this->settleLayoutAfterResize();
    });
    QTimer::singleShot(50, this, [this, layoutGeneration] {
        if (layoutGeneration != this->layoutUpdateGeneration_)
        {
            return;
        }

        this->applySizeConstraints(true);
        this->settleLayoutAfterResize();
    });
}

bool PredictionDialog::isBroadcasterView() const
{
    return this->channel_ != nullptr && this->channel_->isBroadcaster();
}

void PredictionDialog::buildCreateUI()
{
    this->ensureCreateDraft();

    auto *layout = static_cast<QVBoxLayout *>(this->activeWidget_->layout());
    const float rawScale = this->scale();
    const float effectiveScale = contentScale(rawScale);
    const auto uiFont =
        getApp()->getFonts()->getFont(FontStyle::UiMedium, effectiveScale);
    const auto buttonFont =
        getApp()->getFonts()->getFont(FontStyle::UiMediumBold, effectiveScale);
    const auto titleFont = getApp()->getFonts()->getFont(
        FontStyle::UiMediumBold, effectiveScale * 1.08F);
    const QFontMetrics uiMetrics(uiFont);
    const QFontMetrics buttonMetrics(buttonFont);
    const int rowSpacing = std::max(1, int(3 * effectiveScale));
    const int sectionSpacing = std::max(1, int(4 * effectiveScale));
    const int sectionPad = std::max(1, int(4 * effectiveScale));
    const int optionBottomInset = std::max(2, int(std::ceil(2 * rawScale)));
    const int accentWidth = std::max(1, int(2 * effectiveScale));
    const int visibleOutcomeRows =
        std::min(rawScale <= 0.65F   ? 2
                 : rawScale <= 0.85F ? 3
                                     : CREATE_VISIBLE_OUTCOME_ROWS,
                 MAX_OUTCOMES);
    const int outcomeRowHeight = std::max(
        10,
        std::max(int(20 * effectiveScale),
                 uiMetrics.height() + std::max(1, int(2 * effectiveScale))));
    const int compactControlHeight =
        std::max(10, std::max(int(20 * effectiveScale),
                              buttonMetrics.height() +
                                  std::max(1, int(2 * effectiveScale))));
    const int titleInputHeight = std::max(
        10,
        std::max(int(20 * effectiveScale),
                 uiMetrics.height() + std::max(1, int(2 * effectiveScale))));
    const int accentHeight =
        std::max(8, outcomeRowHeight - std::max(2, int(rawScale)));
    const int outcomesListHeight =
        visibleOutcomeRows * outcomeRowHeight +
        std::max(0, visibleOutcomeRows - 1) * rowSpacing + sectionPad * 2 +
        optionBottomInset;

    // ── Title ──────────────────────────────────────────────
    QWidget *previousTabWidget = nullptr;

    auto linkTabOrder = [&previousTabWidget](QWidget *widget) {
        if (widget == nullptr)
        {
            return;
        }

        if (previousTabWidget != nullptr)
        {
            QWidget::setTabOrder(previousTabWidget, widget);
        }
        previousTabWidget = widget;
    };

    auto *titleInput = new PredictionTemplatePicker(this->activeWidget_);
    titleInput->setFont(uiFont);
    titleInput->setFixedHeight(titleInputHeight);
    titleInput->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    titleInput->setToolTip("Open to reuse one of the last 5 predictions.");
    titleInput->lineEdit()->setMaxLength(TITLE_LIMIT);
    titleInput->lineEdit()->setPlaceholderText(
        QString("Prediction title (%1 chars)").arg(TITLE_LIMIT));
    titleInput->setText(this->draftTitle_);
    titleInput->beforeShowPopup = [this, titleInput] {
        return this->populatePredictionTemplates(titleInput);
    };
    QObject::connect(titleInput->lineEdit(), &QLineEdit::textEdited, this,
                     [this](const QString &text) {
                         this->draftTitle_ = text.left(TITLE_LIMIT);
                     });
    titleInput->activated = [this](int templateIndex) {
        if (templateIndex < 0 ||
            templateIndex >= this->predictionTemplates_.size())
        {
            return;
        }

        const auto predictionTemplate =
            this->predictionTemplates_.at(templateIndex);
        this->draftTitle_ = predictionTemplate.title.left(TITLE_LIMIT);
        this->draftOutcomes_ = predictionTemplate.outcomes;
        for (auto &outcome : this->draftOutcomes_)
        {
            outcome = outcome.left(OUTCOME_LIMIT);
        }
        while (this->draftOutcomes_.size() > MAX_OUTCOMES)
        {
            this->draftOutcomes_.removeLast();
        }
        this->draftDurationSeconds_ = predictionTemplate.durationSeconds;
        this->mainScrollValue_ =
            this->scrollArea_->verticalScrollBar()->value();
        this->outcomesScrollValue_ = 0;
        this->ensureCreateDraft();
        this->updateUI();
    };
    if (this->predictionTemplatesFetchedAt_.isValid() &&
        this->predictionTemplatesFetchedAt_.msecsTo(
            QDateTime::currentDateTimeUtc()) < PREDICTION_TEMPLATE_CACHE_MS)
    {
        this->populatePredictionTemplates(titleInput);
    }
    layout->addWidget(titleInput);
    linkTabOrder(titleInput);

    // ── Outcomes header ────────────────────────────────────
    {
        auto *headerRow = new QHBoxLayout();
        headerRow->setSpacing(rowSpacing);
        auto *label = new QLabel("Outcomes");
        label->setObjectName("PredictionSectionTitle");
        label->setFont(buttonFont);
        headerRow->addWidget(label);
        headerRow->addStretch(1);
        auto *count = new QLabel(QString("%1/%2")
                                     .arg(this->draftOutcomes_.size())
                                     .arg(MAX_OUTCOMES));
        count->setObjectName("PredictionCountLabel");
        count->setFont(uiFont);
        headerRow->addWidget(count);
        layout->addLayout(headerRow);
    }

    auto *outcomesPanel = new QWidget(this->activeWidget_);
    outcomesPanel->setObjectName("PredictionOutcomesPanel");
    outcomesPanel->setSizePolicy(QSizePolicy::Preferred,
                                 QSizePolicy::Expanding);
    auto *outcomesPanelLayout = new QVBoxLayout(outcomesPanel);
    outcomesPanelLayout->setContentsMargins(0, 0, 0, 0);
    outcomesPanelLayout->setSpacing(0);

    this->outcomesScrollArea_ = new QScrollArea(outcomesPanel);
    this->outcomesScrollArea_->setObjectName("PredictionOutcomesScrollArea");
    this->outcomesScrollArea_->setFrameShape(QFrame::NoFrame);
    this->outcomesScrollArea_->setWidgetResizable(true);
    this->outcomesScrollArea_->setFocusPolicy(Qt::NoFocus);
    this->outcomesScrollArea_->setHorizontalScrollBarPolicy(
        Qt::ScrollBarAlwaysOff);
    this->outcomesScrollArea_->setVerticalScrollBarPolicy(
        Qt::ScrollBarAlwaysOff);
    this->outcomesScrollArea_->setSizePolicy(QSizePolicy::Expanding,
                                             QSizePolicy::Expanding);
    this->outcomesScrollArea_->setMinimumHeight(outcomesListHeight);
    outcomesPanelLayout->addWidget(this->outcomesScrollArea_);

    auto *outcomesWidget = new QWidget();
    outcomesWidget->setObjectName("PredictionOutcomesContent");
    outcomesWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    auto *outcomesLayout = new QVBoxLayout(outcomesWidget);
    outcomesLayout->setContentsMargins(sectionPad, sectionPad, sectionPad,
                                       sectionPad + optionBottomInset);
    outcomesLayout->setSpacing(rowSpacing);
    outcomesLayout->setSizeConstraint(QLayout::SetMinimumSize);

    // ── Outcome rows (scrolls independently) ───────────────
    for (int i = 0; i < this->draftOutcomes_.size(); ++i)
    {
        auto *rowWidget = new QWidget(outcomesWidget);
        rowWidget->setFixedHeight(outcomeRowHeight);
        auto *row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(rowSpacing);
        row->setAlignment(Qt::AlignVCenter);

        auto *accent = new QWidget(rowWidget);
        accent->setFixedWidth(accentWidth);
        accent->setFixedHeight(accentHeight);
        accent->setStyleSheet(QString("background:%1; border-radius:%2px;")
                                  .arg(outcomeColor(i).name())
                                  .arg(std::max(1, accentWidth / 2)));
        row->addWidget(accent, 0, Qt::AlignVCenter);

        auto *input = new QLineEdit(rowWidget);
        input->setObjectName("PredictionOptionInput");
        input->setMaxLength(OUTCOME_LIMIT);
        input->setPlaceholderText(QString("Option %1").arg(i + 1));
        input->setText(this->draftOutcomes_.at(i));
        input->setFont(uiFont);
        input->setFixedHeight(outcomeRowHeight);
        input->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        QObject::connect(input, &QLineEdit::textChanged, this,
                         [this, i](const QString &text) {
                             if (i < this->draftOutcomes_.size())
                             {
                                 this->draftOutcomes_[i] =
                                     text.left(OUTCOME_LIMIT);
                             }
                         });
        row->addWidget(input, 1, Qt::AlignVCenter);
        linkTabOrder(input);

        outcomesLayout->addWidget(rowWidget);
    }

    outcomesLayout->addStretch(1);
    this->outcomesScrollArea_->setWidget(outcomesWidget);
    QObject::connect(this->outcomesScrollArea_->verticalScrollBar(),
                     &QScrollBar::valueChanged, this, [this](int value) {
                         this->outcomesScrollValue_ = value;
                     });
    QTimer::singleShot(0, this, [this] {
        if (this->outcomesScrollArea_ != nullptr)
        {
            this->outcomesScrollArea_->verticalScrollBar()->setValue(
                this->outcomesScrollValue_);
        }
    });
    layout->addWidget(outcomesPanel, 1);

    {
        auto *outcomeActions = new QHBoxLayout();
        outcomeActions->setSpacing(sectionSpacing);

        auto *addBtn = new QPushButton("Add Outcome", this->activeWidget_);
        addBtn->setObjectName("PredictionCreateAddOptionButton");
        addBtn->setFont(buttonFont);
        addBtn->setFixedHeight(compactControlHeight);
        addBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        addBtn->setEnabled(this->draftOutcomes_.size() < MAX_OUTCOMES);
        QObject::connect(addBtn, &QPushButton::clicked, this, [this] {
            this->mainScrollValue_ =
                this->scrollArea_->verticalScrollBar()->value();
            if (this->outcomesScrollArea_ != nullptr)
            {
                this->outcomesScrollValue_ =
                    this->outcomesScrollArea_->verticalScrollBar()->value();
            }
            this->draftOutcomes_.append(QString());
            this->updateUI();
        });
        outcomeActions->addWidget(addBtn, 1);
        linkTabOrder(addBtn);

        auto *removeBtn =
            new QPushButton("Remove Outcome", this->activeWidget_);
        removeBtn->setObjectName("PredictionCreateRemoveOptionButton");
        removeBtn->setFont(buttonFont);
        removeBtn->setFixedHeight(compactControlHeight);
        removeBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        removeBtn->setEnabled(this->draftOutcomes_.size() > MIN_OUTCOMES);
        QObject::connect(removeBtn, &QPushButton::clicked, this, [this] {
            if (this->draftOutcomes_.size() > MIN_OUTCOMES)
            {
                this->mainScrollValue_ =
                    this->scrollArea_->verticalScrollBar()->value();
                if (this->outcomesScrollArea_ != nullptr)
                {
                    this->outcomesScrollValue_ =
                        this->outcomesScrollArea_->verticalScrollBar()->value();
                }
                this->draftOutcomes_.removeLast();
                this->ensureCreateDraft();
                this->updateUI();
            }
        });
        outcomeActions->addWidget(removeBtn, 1);
        linkTabOrder(removeBtn);

        layout->addLayout(outcomeActions);
    }

    // ── Bottom bar (pinned below scroll area) ──────────────
    const int pad = std::max(1, int(3 * effectiveScale));
    this->bottomWidget_ = new QWidget();
    this->bottomWidget_->setObjectName("PredictionBottomBar");
    this->bottomWidget_->setFont(uiFont);
    this->bottomWidget_->setSizePolicy(QSizePolicy::Preferred,
                                       QSizePolicy::Fixed);
    auto *bottomLayout = new QVBoxLayout(this->bottomWidget_);
    bottomLayout->setContentsMargins(pad, pad, pad, pad);
    bottomLayout->setSpacing(sectionSpacing);

    {
        auto *durationRow = new QHBoxLayout();
        durationRow->setSpacing(rowSpacing);

        auto *durationLabel = new QLabel("Duration");
        durationLabel->setObjectName("PredictionSectionTitle");
        durationLabel->setFont(buttonFont);
        durationRow->addWidget(durationLabel);

        auto *durationCombo = new QComboBox(this->bottomWidget_);
        durationCombo->setObjectName("PredictionCreateDurationCombo");
        durationCombo->setFont(uiFont);
        durationCombo->setFixedHeight(compactControlHeight);
        durationCombo->setSizePolicy(QSizePolicy::Expanding,
                                     QSizePolicy::Fixed);
        int currentIndex = 0;
        for (int i = 0; i < static_cast<int>(std::size(DURATION_OPTIONS)); ++i)
        {
            durationCombo->addItem(DURATION_OPTIONS[i].label,
                                   DURATION_OPTIONS[i].seconds);
            if (DURATION_OPTIONS[i].seconds == this->draftDurationSeconds_)
            {
                currentIndex = i;
            }
        }
        durationCombo->setCurrentIndex(currentIndex);
        QObject::connect(durationCombo,
                         qOverload<int>(&QComboBox::currentIndexChanged), this,
                         [this, durationCombo](int index) {
                             if (index >= 0)
                             {
                                 this->draftDurationSeconds_ =
                                     durationCombo->itemData(index).toInt();
                             }
                         });
        durationRow->addWidget(durationCombo, 1);
        linkTabOrder(durationCombo);
        bottomLayout->addLayout(durationRow);
    }

    {
        auto *actions = new QHBoxLayout();
        actions->setSpacing(rowSpacing);

        auto *startBtn = new QPushButton(
            this->createInFlight_ ? "Starting..." : "Start Prediction",
            this->bottomWidget_);
        startBtn->setObjectName("PredictionCreateStartButton");
        startBtn->setFont(buttonFont);
        startBtn->setFixedHeight(compactControlHeight);
        startBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        startBtn->setEnabled(!this->createInFlight_);
        QObject::connect(startBtn, &QPushButton::clicked, this,
                         &PredictionDialog::startPrediction);
        actions->addWidget(startBtn, 1);
        linkTabOrder(startBtn);
        bottomLayout->addLayout(actions);
    }

    this->mainLayout_->addWidget(this->bottomWidget_);
}

bool PredictionDialog::populatePredictionTemplates(
    PredictionTemplatePicker *picker)
{
    if (picker == nullptr)
    {
        return false;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    const bool hasFreshCache = this->predictionTemplatesFetchedAt_.isValid() &&
                               this->predictionTemplatesFetchedAt_.msecsTo(
                                   now) < PREDICTION_TEMPLATE_CACHE_MS;

    if (hasFreshCache)
    {
        if (picker->property("PredictionTemplatesPopulated").toBool())
        {
            return true;
        }

        if (!this->predictionTemplatesError_.isEmpty())
        {
            picker->setStatusText("Could not load previous predictions");
        }
        else if (this->predictionTemplates_.isEmpty())
        {
            picker->setStatusText("No previous predictions found");
        }
        else
        {
            QStringList labels;
            labels.reserve(this->predictionTemplates_.size());
            for (int i = 0; i < this->predictionTemplates_.size(); ++i)
            {
                labels.push_back(this->predictionTemplates_.at(i).title);
            }
            picker->setItems(labels);
        }
        return true;
    }

    picker->setStatusText("Loading previous predictions...");
    if (!this->predictionTemplatesFetchInFlight_)
    {
        this->fetchPredictionTemplates(picker);
    }

    return true;
}

void PredictionDialog::fetchPredictionTemplates(
    PredictionTemplatePicker *picker)
{
    if (picker == nullptr || this->channel_ == nullptr)
    {
        return;
    }

    this->predictionTemplatesFetchInFlight_ = true;
    QPointer<PredictionTemplatePicker> pickerPtr = picker;
    QPointer<PredictionDialog> self = this;
    QString authError;
    const auto auth = MoltorinoAuth::resolveModerationToken(
        this->channel_->roomId(), this->channel_->getName(), &authError);
    if (!auth.hasToken())
    {
        this->predictionTemplatesFetchInFlight_ = false;
        picker->setStatusText(
            authError.isEmpty()
                ? moltorinoAuthRequiredMessage("loading previous predictions")
                : authError);
        return;
    }

    TwitchGql::getPredictionTemplates(
        this->channel_->getName(), auth.token,
        [self, pickerPtr](QVector<PredictionTemplate> templates) {
            if (!self)
            {
                return;
            }

            self->predictionTemplatesFetchInFlight_ = false;
            self->predictionTemplates_ = std::move(templates);
            self->predictionTemplatesError_.clear();
            self->predictionTemplatesFetchedAt_ =
                QDateTime::currentDateTimeUtc();
            if (pickerPtr)
            {
                pickerPtr->setProperty("PredictionTemplatesPopulated", false);
                self->populatePredictionTemplates(pickerPtr);
            }
        },
        [self, pickerPtr](const QString &error) {
            if (!self)
            {
                return;
            }

            self->predictionTemplatesFetchInFlight_ = false;
            self->predictionTemplates_.clear();
            self->predictionTemplatesError_ =
                error.isEmpty() ? "Could not load previous predictions"
                                : normalizeMoltorinoAuthError(
                                      "loading previous predictions", error);
            self->predictionTemplatesFetchedAt_ =
                QDateTime::currentDateTimeUtc();
            if (pickerPtr)
            {
                pickerPtr->setProperty("PredictionTemplatesPopulated", false);
                self->populatePredictionTemplates(pickerPtr);
            }
        });
}

void PredictionDialog::startPrediction()
{
    const auto title = this->draftTitle_.trimmed();
    if (title.isEmpty())
    {
        this->channel_->addSystemMessage("Prediction title cannot be empty.");
        return;
    }

    QStringList choices;
    QSet<QString> deduplicatedChoices;
    for (const auto &draftOutcome : this->draftOutcomes_)
    {
        const auto trimmed = draftOutcome.trimmed();
        if (trimmed.isEmpty())
        {
            continue;
        }
        choices.push_back(trimmed);
        deduplicatedChoices.insert(trimmed.toCaseFolded());
    }

    if (choices.size() < MIN_OUTCOMES)
    {
        this->channel_->addSystemMessage(
            "Please provide at least two prediction options.");
        return;
    }
    if (choices.size() > MAX_OUTCOMES)
    {
        this->channel_->addSystemMessage(
            QString("Predictions can have at most %1 options.")
                .arg(MAX_OUTCOMES));
        return;
    }
    if (deduplicatedChoices.size() != choices.size())
    {
        this->channel_->addSystemMessage(
            "Prediction options must be different from each other.");
        return;
    }

    QString authError;
    const auto auth = MoltorinoAuth::resolveModerationToken(
        this->channel_->roomId(), this->channel_->getName(), &authError);
    if (!auth.hasToken())
    {
        this->channel_->addSystemMessage(
            authError.isEmpty()
                ? moltorinoAuthRequiredMessage("creating predictions")
                : authError);
        return;
    }

    this->createInFlight_ = true;
    this->updateUI();

    QPointer<PredictionDialog> self = this;
    TwitchGql::createPredictionEvent(
        this->channel_->roomId(), title, choices, this->draftDurationSeconds_,
        auth.token,
        [self, title] {
            if (!self)
            {
                return;
            }

            /*
            self->channel_->addSystemMessage(
                QString("Created prediction: '%1'").arg(title));
            */
            self->close();
        },
        [self](const QString &error) {
            if (!self)
            {
                return;
            }

            self->createInFlight_ = false;
            self->channel_->addSystemMessage(
                "Failed to create prediction: " +
                normalizePredictionCreateError(error));
            self->updateUI();
        });
}

void PredictionDialog::buildManageUI()
{
    auto *layout = static_cast<QVBoxLayout *>(this->activeWidget_->layout());
    const auto &prediction = *this->currentPrediction_;
    const float rawScale = this->scale();
    const float effectiveScale = contentScale(rawScale);
    const auto uiFont =
        getApp()->getFonts()->getFont(FontStyle::UiMedium, effectiveScale);
    const auto buttonFont =
        getApp()->getFonts()->getFont(FontStyle::UiMediumBold, effectiveScale);
    const auto titleFont = getApp()->getFonts()->getFont(
        FontStyle::UiMediumBold, effectiveScale * 1.18F);
    const QFontMetrics uiMetrics(uiFont);
    const QFontMetrics buttonMetrics(buttonFont);
    const int rowSpacing = std::max(1, int(3 * effectiveScale));
    const int sectionPad = std::max(1, int(4 * effectiveScale));
    const int accentWidth = std::max(1, int(2 * effectiveScale));
    const int compactControlHeight =
        std::max(10, std::max(int(20 * effectiveScale),
                              buttonMetrics.height() +
                                  std::max(1, int(2 * effectiveScale))));
    const int outcomeRowHeight = std::max(
        10,
        std::max(int(20 * effectiveScale),
                 uiMetrics.height() + std::max(1, int(2 * effectiveScale))));
    const int accentHeight =
        std::max(8, outcomeRowHeight - std::max(2, int(rawScale)));
    const int visibleOutcomeRows = rawScale <= 0.65F ? 2
                                   : rawScale <= 0.85F
                                       ? 4
                                       : MANAGE_VISIBLE_OUTCOME_ROWS;
    const int outcomesListHeight =
        visibleOutcomeRows * outcomeRowHeight +
        std::max(0, visibleOutcomeRows - 1) * rowSpacing + sectionPad * 2;
    const auto locale = QLocale();

    // ── Title ──────────────────────────────────────────────
    auto *titleLabel = new QLabel(prediction.title, this->activeWidget_);
    titleLabel->setObjectName("PredictionCurrentTitle");
    titleLabel->setFont(titleFont);
    titleLabel->setWordWrap(true);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setMinimumSize(1, 1);
    titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(titleLabel);

    // ── Outcomes header ────────────────────────────────────
    {
        auto *headerRow = new QHBoxLayout();
        headerRow->setSpacing(rowSpacing);
        auto *label = new QLabel("Outcomes");
        label->setObjectName("PredictionSectionTitle");
        label->setFont(buttonFont);
        headerRow->addWidget(label);
        headerRow->addStretch(1);
        auto *count = new QLabel(QString::number(prediction.outcomes.size()));
        count->setObjectName("PredictionCountLabel");
        count->setFont(uiFont);
        headerRow->addWidget(count);
        layout->addLayout(headerRow);
    }

    // ── Outcomes panel ─────────────────────────────────────
    qlonglong totalPoints = 0;
    for (const auto &outcome : prediction.outcomes)
    {
        totalPoints += outcome.totalPoints;
    }
    if (this->selectedManageOutcomeId_.isEmpty() ||
        findPredictionOutcome(prediction, this->selectedManageOutcomeId_) ==
            nullptr)
    {
        this->selectedManageOutcomeId_ = prediction.outcomes.empty()
                                             ? QString()
                                             : prediction.outcomes.front().id;
    }

    auto *outcomesPanel = new QWidget(this->activeWidget_);
    outcomesPanel->setObjectName("PredictionOutcomesPanel");
    outcomesPanel->setSizePolicy(QSizePolicy::Preferred,
                                 QSizePolicy::Expanding);
    auto *outcomesPanelLayout = new QVBoxLayout(outcomesPanel);
    outcomesPanelLayout->setContentsMargins(0, 0, 0, 0);
    outcomesPanelLayout->setSpacing(0);

    this->outcomesScrollArea_ = new QScrollArea(outcomesPanel);
    this->outcomesScrollArea_->setObjectName("PredictionOutcomesScrollArea");
    this->outcomesScrollArea_->setFrameShape(QFrame::NoFrame);
    this->outcomesScrollArea_->setWidgetResizable(true);
    this->outcomesScrollArea_->setHorizontalScrollBarPolicy(
        Qt::ScrollBarAlwaysOff);
    this->outcomesScrollArea_->setVerticalScrollBarPolicy(
        Qt::ScrollBarAsNeeded);
    this->outcomesScrollArea_->setSizePolicy(QSizePolicy::Expanding,
                                             QSizePolicy::Fixed);
    this->outcomesScrollArea_->setMinimumHeight(outcomesListHeight);
    this->outcomesScrollArea_->setMaximumHeight(outcomesListHeight);
    outcomesPanelLayout->addWidget(this->outcomesScrollArea_);

    auto *outcomesWidget = new QWidget();
    outcomesWidget->setObjectName("PredictionOutcomesContent");
    outcomesWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    auto *outcomesLayout = new QVBoxLayout(outcomesWidget);
    outcomesLayout->setContentsMargins(sectionPad, sectionPad, sectionPad,
                                       sectionPad);
    outcomesLayout->setSpacing(rowSpacing);
    outcomesLayout->setAlignment(Qt::AlignTop);
    outcomesLayout->setSizeConstraint(QLayout::SetMinimumSize);

    for (int i = 0; i < static_cast<int>(prediction.outcomes.size()); ++i)
    {
        const auto &outcome = prediction.outcomes.at(i);
        const int percentage =
            totalPoints > 0
                ? static_cast<int>((outcome.totalPoints * 100.0) / totalPoints)
                : 0;

        auto *rowWidget = new QWidget(outcomesWidget);
        rowWidget->setFixedHeight(outcomeRowHeight);
        rowWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto *row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(rowSpacing);
        row->setAlignment(Qt::AlignVCenter);

        auto *accent = new QWidget(rowWidget);
        accent->setFixedWidth(accentWidth);
        accent->setFixedHeight(accentHeight);
        accent->setProperty("predictionOutcomeId", outcome.id);
        accent->installEventFilter(this);
        accent->setStyleSheet(QString("background:%1; border-radius:%2px;")
                                  .arg(outcomeColor(i, outcome.color).name())
                                  .arg(std::max(1, accentWidth / 2)));
        row->addWidget(accent, 0, Qt::AlignVCenter);

        rowWidget->setProperty("predictionOutcomeId", outcome.id);
        rowWidget->installEventFilter(this);
        auto *card = new QWidget(rowWidget);
        card->setObjectName("PredictionOutcomeRowCard");
        card->setFixedHeight(outcomeRowHeight);
        card->setCursor(Qt::PointingHandCursor);
        card->setProperty("predictionOutcomeId", outcome.id);
        card->setProperty("selectedManageOutcome",
                          outcome.id == this->selectedManageOutcomeId_);
        card->installEventFilter(this);
        auto *cardLayout = new QHBoxLayout(card);
        cardLayout->setContentsMargins(sectionPad, 0, sectionPad, 0);
        cardLayout->setSpacing(rowSpacing);

        auto *nameLabel = new QLabel(outcome.title, card);
        nameLabel->setObjectName("PredictionOutcomeName");
        nameLabel->setFont(buttonFont);
        nameLabel->setToolTip(outcome.title);
        nameLabel->setMinimumWidth(0);
        nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        nameLabel->setProperty("predictionOutcomeId", outcome.id);
        nameLabel->installEventFilter(this);
        cardLayout->addWidget(nameLabel, 1, Qt::AlignVCenter);

        auto *pctLabel = new QLabel(QString::number(percentage) + "%", card);
        pctLabel->setObjectName("PredictionOutcomeStats");
        pctLabel->setFont(buttonFont);
        pctLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        pctLabel->setFixedWidth(
            QFontMetrics(buttonFont).horizontalAdvance("100%") +
            std::max(4, int(6 * effectiveScale)));
        cardLayout->addWidget(pctLabel, 0, Qt::AlignVCenter);

        row->addWidget(card, 1);
        outcomesLayout->addWidget(rowWidget);
    }

    this->outcomesScrollArea_->setWidget(outcomesWidget);
    QObject::connect(this->outcomesScrollArea_->verticalScrollBar(),
                     &QScrollBar::valueChanged, this, [this](int value) {
                         this->outcomesScrollValue_ = value;
                     });
    QTimer::singleShot(0, this, [this] {
        if (this->outcomesScrollArea_ != nullptr)
        {
            this->outcomesScrollArea_->verticalScrollBar()->setValue(
                this->outcomesScrollValue_);
        }
    });
    layout->addWidget(outcomesPanel);

    // ── Bottom bar (pinned below scroll area) ──────────────
    const int pad = std::max(1, int(3 * effectiveScale));
    this->bottomWidget_ = new QWidget();
    this->bottomWidget_->setObjectName("PredictionBottomBar");
    this->bottomWidget_->setFont(uiFont);
    this->bottomWidget_->setSizePolicy(QSizePolicy::Preferred,
                                       QSizePolicy::Fixed);
    auto *bottomLayout = new QVBoxLayout(this->bottomWidget_);
    bottomLayout->setContentsMargins(pad, pad, pad, pad);
    bottomLayout->setSpacing(rowSpacing);

    const auto predictionId = prediction.id;
    auto makeCancelButton = [this, predictionId, compactControlHeight,
                             buttonFont]() {
        auto *cancelButton = new QPushButton("Delete", this->bottomWidget_);
        cancelButton->setObjectName("PredictionManageDangerButton");
        cancelButton->setFont(buttonFont);
        cancelButton->setFixedHeight(compactControlHeight);
        cancelButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        QObject::connect(
            cancelButton, &QPushButton::clicked, this,
            [this, predictionId, cancelButton] {
                QString authError;
                const auto auth = MoltorinoAuth::resolveModerationToken(
                    this->channel_->roomId(), this->channel_->getName(),
                    &authError);
                if (!auth.hasToken())
                {
                    this->channel_->addSystemMessage(
                        authError.isEmpty() ? moltorinoAuthRequiredMessage(
                                                  "deleting predictions")
                                            : authError);
                    return;
                }

                cancelButton->setEnabled(false);
                cancelButton->setText("Deleting...");

                QPointer<PredictionDialog> self = this;
                QPointer<QPushButton> button = cancelButton;
                TwitchGql::cancelPrediction(
                    predictionId, auth.token,
                    [self] {
                        if (!self)
                        {
                            return;
                        }
                        /*
                                     self->channel_->addSystemMessage(
                                         "Prediction deleted — points refunded.");
                                     */
                        self->close();
                    },
                    [self, button](const QString &error) {
                        if (!self)
                        {
                            return;
                        }
                        self->channel_->addSystemMessage(
                            "Failed to delete prediction: " +
                            normalizeMoltorinoAuthError("deleting predictions",
                                                        error));
                        if (button)
                        {
                            button->setEnabled(true);
                            button->setText("Delete");
                        }
                    });
            });
        return cancelButton;
    };

    if (prediction.status == "ACTIVE")
    {
        auto *lockButton =
            new QPushButton("Lock Submissions", this->bottomWidget_);
        lockButton->setObjectName("PredictionManagePrimaryButton");
        lockButton->setFont(buttonFont);
        lockButton->setFixedHeight(compactControlHeight);
        lockButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        QObject::connect(
            lockButton, &QPushButton::clicked, this,
            [this, predictionId, lockButton] {
                QString authError;
                const auto auth = MoltorinoAuth::resolveModerationToken(
                    this->channel_->roomId(), this->channel_->getName(),
                    &authError);
                if (!auth.hasToken())
                {
                    this->channel_->addSystemMessage(
                        authError.isEmpty() ? moltorinoAuthRequiredMessage(
                                                  "locking predictions")
                                            : authError);
                    return;
                }

                lockButton->setEnabled(false);
                lockButton->setText("Locking...");

                QPointer<PredictionDialog> self = this;
                QPointer<QPushButton> button = lockButton;
                TwitchGql::lockPrediction(
                    predictionId, auth.token,
                    [self] {
                        if (!self)
                        {
                            return;
                        }
                    },
                    [self, button](const QString &error) {
                        if (!self)
                        {
                            return;
                        }
                        self->channel_->addSystemMessage(
                            "Failed to lock prediction: " +
                            normalizeMoltorinoAuthError("locking predictions",
                                                        error));
                        if (button)
                        {
                            button->setEnabled(true);
                            button->setText("Lock Submissions");
                        }
                    });
            });

        auto *actionsRow = new QHBoxLayout();
        actionsRow->setSpacing(rowSpacing);
        actionsRow->addWidget(makeCancelButton());
        actionsRow->addWidget(lockButton);
        bottomLayout->addLayout(actionsRow);
    }
    else
    {
        auto *resolveCombo = new QComboBox(this->bottomWidget_);
        resolveCombo->setObjectName("PredictionManageResolveCombo");
        resolveCombo->setFont(uiFont);
        resolveCombo->setFixedHeight(compactControlHeight);
        resolveCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        this->manageResolveCombo_ = resolveCombo;
        for (const auto &outcome : prediction.outcomes)
        {
            resolveCombo->addItem(outcome.title, outcome.id);
        }
        const int selectedIndex =
            resolveCombo->findData(this->selectedManageOutcomeId_);
        if (selectedIndex >= 0)
        {
            resolveCombo->setCurrentIndex(selectedIndex);
        }
        QObject::connect(resolveCombo,
                         qOverload<int>(&QComboBox::currentIndexChanged), this,
                         [this, resolveCombo](int) {
                             this->selectedManageOutcomeId_ =
                                 resolveCombo->currentData().toString();
                             this->updateManageOutcomeSelection();
                         });
        bottomLayout->addWidget(resolveCombo);

        auto *resolveButton = new QPushButton("Complete", this->bottomWidget_);
        resolveButton->setObjectName("PredictionManagePrimaryButton");
        resolveButton->setFont(buttonFont);
        resolveButton->setFixedHeight(compactControlHeight);
        resolveButton->setSizePolicy(QSizePolicy::Expanding,
                                     QSizePolicy::Fixed);
        QObject::connect(
            resolveButton, &QPushButton::clicked, this,
            [this, predictionId, resolveCombo, resolveButton] {
                const auto winnerId = resolveCombo->currentData().toString();
                const auto winnerTitle = resolveCombo->currentText();
                if (winnerId.isEmpty())
                {
                    return;
                }

                QString authError;
                const auto auth = MoltorinoAuth::resolveModerationToken(
                    this->channel_->roomId(), this->channel_->getName(),
                    &authError);
                if (!auth.hasToken())
                {
                    this->channel_->addSystemMessage(
                        authError.isEmpty() ? moltorinoAuthRequiredMessage(
                                                  "completing predictions")
                                            : authError);
                    return;
                }

                resolveButton->setEnabled(false);
                resolveButton->setText("Resolving...");

                QPointer<PredictionDialog> self = this;
                QPointer<QPushButton> button = resolveButton;
                TwitchGql::resolvePrediction(
                    predictionId, winnerId, auth.token,
                    [self, winnerTitle] {
                        if (!self)
                        {
                            return;
                        }

                        /*
                                     self->channel_->addSystemMessage(
                                         QString("Prediction resolved: \"%1\" wins!")
                                             .arg(winnerTitle));
                                     */
                        self->close();
                    },
                    [self, button](const QString &error) {
                        if (!self)
                        {
                            return;
                        }
                        self->channel_->addSystemMessage(
                            "Failed to resolve prediction: " +
                            normalizeMoltorinoAuthError(
                                "completing predictions", error));
                        if (button)
                        {
                            button->setEnabled(true);
                            button->setText("Complete");
                        }
                    });
            });

        auto *actionsRow = new QHBoxLayout();
        actionsRow->setSpacing(rowSpacing);

        actionsRow->addWidget(makeCancelButton());
        actionsRow->addWidget(resolveButton);
        bottomLayout->addLayout(actionsRow);
    }

    layout->addStretch(1);
    this->mainLayout_->addWidget(this->bottomWidget_);
}

void PredictionDialog::buildBettingUI()
{
    auto *layout = static_cast<QVBoxLayout *>(this->activeWidget_->layout());
    const auto &prediction = *this->currentPrediction_;
    const bool broadcasterView = this->isBroadcasterView();
    const float rawScale = this->scale();
    const float effectiveScale = contentScale(rawScale);
    const auto uiFont =
        getApp()->getFonts()->getFont(FontStyle::UiMedium, effectiveScale);
    const auto buttonFont =
        getApp()->getFonts()->getFont(FontStyle::UiMediumBold, effectiveScale);
    const auto titleFont = getApp()->getFonts()->getFont(
        FontStyle::UiMediumBold, effectiveScale * 1.18F);
    const auto optionNameFont = getApp()->getFonts()->getFont(
        FontStyle::UiMediumBold, effectiveScale * 0.94F);
    const auto statFont = getApp()->getFonts()->getFont(FontStyle::UiMedium,
                                                        effectiveScale * 0.92F);
    const QFontMetrics buttonMetrics(buttonFont);
    const int rowSpacing = std::max(1, int(3 * effectiveScale));
    const int sectionSpacing = std::max(1, int(4 * effectiveScale));
    const int sectionPad = std::max(1, int(4 * effectiveScale));
    const int contentSectionGap = std::max(1, int(2 * rawScale));
    const int compactControlHeight =
        std::max(10, std::max(int(20 * effectiveScale),
                              buttonMetrics.height() +
                                  std::max(1, int(2 * effectiveScale))));
    const auto locale = QLocale();

    qlonglong totalPoints = 0;
    for (const auto &outcome : prediction.outcomes)
    {
        totalPoints += outcome.totalPoints;
    }

    const bool multiOutcomeBetting = prediction.outcomes.size() > 2;
    const TwitchChannel::PredictionOutcome *selectedOutcome =
        (!broadcasterView && multiOutcomeBetting &&
         !this->selectedBettingOutcomeId_.isEmpty())
            ? findPredictionOutcome(prediction, this->selectedBettingOutcomeId_)
            : nullptr;
    int selectedOutcomeIndex = -1;
    if (selectedOutcome != nullptr)
    {
        for (int i = 0; i < static_cast<int>(prediction.outcomes.size()); ++i)
        {
            if (prediction.outcomes.at(i).id == selectedOutcome->id)
            {
                selectedOutcomeIndex = i;
                break;
            }
        }
    }

    // ── Question card (title + countdown) ─────────────────
    auto *questionCard = new QWidget(this->activeWidget_);
    questionCard->setObjectName("PredictionQuestionCard");
    questionCard->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *questionCardLayout = new QVBoxLayout(questionCard);
    const int questionPad = std::max(1, int(4 * rawScale));
    questionCardLayout->setContentsMargins(
        questionPad * 2, std::max(2, int(5 * effectiveScale)), questionPad * 2,
        std::max(1, int(3 * effectiveScale)));
    questionCardLayout->setSpacing(std::max(1, int(2 * effectiveScale)));

    auto *titleLabel = new QLabel(prediction.title, questionCard);
    titleLabel->setObjectName("PredictionCurrentTitle");
    titleLabel->setFont(titleFont);
    titleLabel->setWordWrap(true);
    titleLabel->setAlignment(Qt::AlignCenter);
    questionCardLayout->addWidget(titleLabel);

    {
        auto *statusLabel = new QLabel(questionCard);
        statusLabel->setObjectName("PredictionCountdown");
        statusLabel->setFont(uiFont);
        statusLabel->setAlignment(Qt::AlignCenter);
        questionCardLayout->addWidget(statusLabel);

        if (prediction.status == "ACTIVE")
        {
            QPointer<QLabel> labelPtr = statusLabel;
            auto createdAt = prediction.createdAt;
            int windowSecs = prediction.predictionWindowSeconds;
            if (!createdAt.isValid())
            {
                createdAt = QDateTime::currentDateTimeUtc();
            }
            if (windowSecs <= 0)
            {
                windowSecs = 120;
            }
            auto updateCountdown = [labelPtr, createdAt, windowSecs]() {
                if (!labelPtr)
                {
                    return;
                }
                const auto now = QDateTime::currentDateTimeUtc();
                const int elapsed = static_cast<int>(createdAt.secsTo(now));
                const int remaining = std::max(0, windowSecs - elapsed);
                if (remaining > 0)
                {
                    labelPtr->setText(QString("Closing in %1")
                                          .arg(formatRemainingTime(remaining)));
                }
                else
                {
                    labelPtr->setText("Submissions closing...");
                }
            };
            updateCountdown();

            auto *timer = new QTimer(questionCard);
            timer->setInterval(1000);
            QObject::connect(timer, &QTimer::timeout, statusLabel,
                             updateCountdown);
            timer->start();
        }
        else if (prediction.status == "LOCKED")
        {
            statusLabel->setText("Submissions locked");
        }
        else
        {
            statusLabel->setText(formatStatus(prediction.status).toLower());
        }
    }
    questionCardLayout->activate();
    questionCard->setFixedHeight(questionCardLayout->sizeHint().height());
    layout->addWidget(questionCard);
    layout->addSpacing(contentSectionGap);

    // ── Two-option card layout ─────────────────────────────
    if (prediction.outcomes.size() == 2)
    {
        auto *cardsContainer = new QWidget(this->activeWidget_);
        cardsContainer->setSizePolicy(QSizePolicy::Preferred,
                                      QSizePolicy::Fixed);
        auto *cardsRow = new QHBoxLayout(cardsContainer);
        cardsRow->setContentsMargins(sectionPad, std::max(1, rowSpacing / 2),
                                     sectionPad, std::max(1, rowSpacing / 2));
        cardsRow->setSpacing(std::max(sectionSpacing, int(8 * effectiveScale)));

        const auto largePctFont = getApp()->getFonts()->getFont(
            FontStyle::UiMediumBold, effectiveScale * 2.65F);

        for (int i = 0; i < 2; ++i)
        {
            const auto &outcome = prediction.outcomes.at(i);
            const QColor color = outcomeColor(i, outcome.color);
            const int percentage =
                totalPoints > 0
                    ? static_cast<int>((outcome.totalPoints * 100.0) /
                                       totalPoints)
                    : 0;
            const double multiplier =
                outcome.totalPoints > 0
                    ? static_cast<double>(totalPoints) / outcome.totalPoints
                    : 0.0;
            const bool isLeft = (i == 0);

            auto *halfWidget = new QWidget(cardsContainer);
            halfWidget->setSizePolicy(QSizePolicy::Preferred,
                                      QSizePolicy::Fixed);
            auto *halfLayout = new QHBoxLayout(halfWidget);
            halfLayout->setContentsMargins(0, 0, 0, 0);
            halfLayout->setSpacing(
                std::max(rowSpacing, int(4 * effectiveScale)));

            // ── Stats Block ──
            auto *statsWidget = new QWidget(halfWidget);
            statsWidget->setSizePolicy(QSizePolicy::Preferred,
                                       QSizePolicy::Fixed);
            auto *statsLayout = new QVBoxLayout(statsWidget);
            statsLayout->setContentsMargins(0, 0, 0, 0);
            statsLayout->setSpacing(std::max(1, int(1.5 * effectiveScale)));

            // icons: ○ = points, ⚔ = multiplier, ⚑ = users, ♛ = pool share
            auto addStatRow = [&](const QString &svgData, const QString &value,
                                  const QString &tooltip = QString(),
                                  const QString &statRole = QString()) {
                auto *row = new QWidget(statsWidget);
                auto *rowLay = new QHBoxLayout(row);
                rowLay->setContentsMargins(0, 0, 0, 0);
                rowLay->setSpacing(rowSpacing);

                auto *iconLabel = new QLabel(row);
                const int iconSize = std::max(10, int(12 * effectiveScale));
                iconLabel->setPixmap(renderSvgIcon(svgData, color, iconSize));
                iconLabel->setFixedSize(iconSize, iconSize);

                auto *valLabel = new QLabel(value, row);
                valLabel->setObjectName("PredictionBetStatValue");
                valLabel->setProperty("outcomeId", outcome.id);
                valLabel->setProperty("statRole", statRole);
                valLabel->setFont(statFont);
                valLabel->setStyleSheet(
                    "background: transparent; border: none;");

                if (!tooltip.isEmpty())
                {
                    valLabel->setToolTip(tooltip);
                }

                if (isLeft)
                {
                    rowLay->addWidget(iconLabel, 0, Qt::AlignVCenter);
                    rowLay->addWidget(valLabel, 1,
                                      Qt::AlignLeft | Qt::AlignVCenter);
                }
                else
                {
                    rowLay->addStretch(1);
                    rowLay->addWidget(valLabel, 0,
                                      Qt::AlignRight | Qt::AlignVCenter);
                    rowLay->addWidget(iconLabel, 0, Qt::AlignVCenter);
                }

                statsLayout->addWidget(row);
            };

            addStatRow(SVG_POINTS, formatCompact(outcome.totalPoints),
                       QString(), "points");
            addStatRow(SVG_TROPHY, QString::number(multiplier, 'f', 1) + "x",
                       QString(), "multiplier");
            addStatRow(SVG_USERS, locale.toString(outcome.totalUsers),
                       QString(), "users");
            addStatRow(SVG_CROWN, formatCompact(outcome.topPoints),
                       outcome.topPredictorName, "crown");

            // ── Center/Inner Block (Title, Pct, Bar) ──
            auto *innerWidget = new QWidget(halfWidget);
            innerWidget->setSizePolicy(QSizePolicy::Preferred,
                                       QSizePolicy::Fixed);
            auto *innerLayout = new QVBoxLayout(innerWidget);
            innerLayout->setContentsMargins(
                0, std::max(2, int(4 * effectiveScale)), 0,
                std::max(2, int(4 * effectiveScale)));
            innerLayout->setSpacing(0);

            auto *nameLabel = new QLabel(outcome.title, innerWidget);
            nameLabel->setFont(optionNameFont);
            nameLabel->setAlignment(isLeft
                                        ? (Qt::AlignRight | Qt::AlignVCenter)
                                        : (Qt::AlignLeft | Qt::AlignVCenter));
            nameLabel->setWordWrap(false);
            nameLabel->setMinimumWidth(
                QFontMetrics(optionNameFont).horizontalAdvance(outcome.title));
            nameLabel->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
            nameLabel->setStyleSheet(
                QString("color: %1; font-weight: 700; background: transparent; "
                        "border: none;")
                    .arg(color.name()));
            nameLabel->setToolTip(outcome.title);
            innerLayout->addWidget(nameLabel, 0,
                                   isLeft ? Qt::AlignRight : Qt::AlignLeft);

            auto *pctLabel =
                new QLabel(QString::number(percentage) + "%", innerWidget);
            pctLabel->setObjectName("PredictionPct_" + outcome.id);
            pctLabel->setFont(largePctFont);
            pctLabel->setAlignment(isLeft ? (Qt::AlignRight | Qt::AlignVCenter)
                                          : (Qt::AlignLeft | Qt::AlignVCenter));
            pctLabel->setStyleSheet(
                QString("color: %1; font-weight: 700; background: transparent; "
                        "border: none;")
                    .arg(color.name()));
            innerLayout->addWidget(pctLabel, 0,
                                   isLeft ? Qt::AlignRight : Qt::AlignLeft);

            auto *barContainer = new QWidget(innerWidget);
            auto *barLay = new QHBoxLayout(barContainer);
            barLay->setContentsMargins(0, 0, 0, 0);
            barLay->setSpacing(0);

            int fillW = std::max(
                16,
                int(80 * (std::max(1, percentage) / 100.0) * effectiveScale));
            auto *filledBar = new QWidget(barContainer);
            filledBar->setObjectName("PredictionBar_" + outcome.id);
            filledBar->setFixedSize(fillW,
                                    std::max(4, int(5 * effectiveScale)));
            filledBar->setStyleSheet(
                QString("background: %1; border-radius: %2px;")
                    .arg(color.name())
                    .arg(std::max(2, int(2.5 * effectiveScale))));

            if (isLeft)
            {
                barLay->addStretch(1);
                barLay->addWidget(filledBar, 0, Qt::AlignRight);
            }
            else
            {
                barLay->addWidget(filledBar, 0, Qt::AlignLeft);
                barLay->addStretch(1);
            }
            innerLayout->addWidget(barContainer, 0,
                                   isLeft ? Qt::AlignRight : Qt::AlignLeft);

            if (isLeft)
            {
                halfLayout->addWidget(statsWidget, 0,
                                      Qt::AlignLeft | Qt::AlignBottom);
                halfLayout->addStretch(1);
                halfLayout->addWidget(innerWidget, 0,
                                      Qt::AlignRight | Qt::AlignTop);
            }
            else
            {
                halfLayout->addWidget(innerWidget, 0,
                                      Qt::AlignLeft | Qt::AlignTop);
                halfLayout->addStretch(1);
                halfLayout->addWidget(statsWidget, 0,
                                      Qt::AlignRight | Qt::AlignBottom);
            }

            cardsRow->addWidget(halfWidget, 1);

            if (isLeft)
            {
                auto *dividerContainer = new QWidget(cardsContainer);
                auto *dividerLayout = new QVBoxLayout(dividerContainer);
                dividerLayout->setContentsMargins(0, 0, 0, 0);
                auto *divider = new QWidget(dividerContainer);
                divider->setObjectName("PredictionDivider");
                divider->setFixedWidth(1);
                divider->setFixedHeight(std::max(40, int(52 * effectiveScale)));
                dividerLayout->addWidget(divider, 0,
                                         Qt::AlignHCenter | Qt::AlignBottom);
                cardsRow->addWidget(dividerContainer, 0);
            }
        }

        layout->addWidget(cardsContainer);
    }
    else
    {
        if (selectedOutcome == nullptr)
        {
            const auto compactMetaFont = getApp()->getFonts()->getFont(
                FontStyle::UiMedium, effectiveScale * 0.82F);
            const auto compactPctFont = getApp()->getFonts()->getFont(
                FontStyle::UiMediumBold, effectiveScale * 1.08F);
            const QFontMetrics uiMetrics(uiFont);
            const int accentWidth = std::max(2, int(3 * effectiveScale));
            const int outcomeRowHeight = std::max(
                10, std::max(int(26 * effectiveScale),
                             uiMetrics.height() +
                                 std::max(6, int(7 * effectiveScale))));
            const int accentHeight = std::max(
                12, outcomeRowHeight - std::max(6, int(8 * effectiveScale)));
            const int visibleRows = rawScale <= 0.65F ? 4 : 5;
            const int listHeight = visibleRows * outcomeRowHeight +
                                   std::max(0, visibleRows - 1) * rowSpacing +
                                   sectionPad * 2;

            auto *outcomesPanel = new QWidget(this->activeWidget_);
            this->bettingOutcomesPanel_ = outcomesPanel;
            outcomesPanel->setObjectName("PredictionOutcomesPanel");
            outcomesPanel->setMinimumWidth(0);
            outcomesPanel->setSizePolicy(QSizePolicy::Preferred,
                                         QSizePolicy::Expanding);
            outcomesPanel->setProperty("predictionWantedHeight", listHeight);
            outcomesPanel->setMinimumHeight(listHeight);
            outcomesPanel->setMaximumHeight(listHeight);
            auto *outcomesPanelLayout = new QVBoxLayout(outcomesPanel);
            outcomesPanelLayout->setContentsMargins(0, 0, 0, 0);
            outcomesPanelLayout->setSpacing(0);

            this->outcomesScrollArea_ = new QScrollArea(outcomesPanel);
            this->outcomesScrollArea_->setObjectName(
                "PredictionOutcomesScrollArea");
            this->outcomesScrollArea_->setFrameShape(QFrame::NoFrame);
            this->outcomesScrollArea_->setWidgetResizable(true);
            this->outcomesScrollArea_->setHorizontalScrollBarPolicy(
                Qt::ScrollBarAlwaysOff);
            this->outcomesScrollArea_->setVerticalScrollBarPolicy(
                Qt::ScrollBarAsNeeded);
            this->outcomesScrollArea_->setSizePolicy(QSizePolicy::Expanding,
                                                     QSizePolicy::Expanding);
            outcomesPanelLayout->addWidget(this->outcomesScrollArea_);

            auto *outcomesWidget = new QWidget();
            outcomesWidget->setObjectName("PredictionOutcomesContent");
            outcomesWidget->setSizePolicy(QSizePolicy::Preferred,
                                          QSizePolicy::Minimum);
            auto *outcomesLayout = new QVBoxLayout(outcomesWidget);
            outcomesLayout->setContentsMargins(sectionPad, 0, sectionPad,
                                               sectionPad);
            outcomesLayout->setSpacing(rowSpacing);
            outcomesLayout->setSizeConstraint(QLayout::SetMinimumSize);

            for (int i = 0; i < static_cast<int>(prediction.outcomes.size());
                 ++i)
            {
                const auto &outcome = prediction.outcomes.at(i);
                const QColor color = outcomeColor(i, outcome.color);
                const int pct =
                    totalPoints > 0
                        ? static_cast<int>((outcome.totalPoints * 100.0) /
                                           totalPoints)
                        : 0;
                const double multiplier =
                    outcome.totalPoints > 0
                        ? static_cast<double>(totalPoints) / outcome.totalPoints
                        : 0.0;

                auto installSelectionTarget = [&](QObject *target) {
                    if (broadcasterView)
                    {
                        return;
                    }
                    target->setProperty("predictionBetOutcomeId", outcome.id);
                    target->installEventFilter(this);
                };

                auto *rowWidget = new QWidget(outcomesWidget);
                rowWidget->setFixedHeight(outcomeRowHeight);
                rowWidget->setCursor(broadcasterView ? Qt::ArrowCursor
                                                     : Qt::PointingHandCursor);
                installSelectionTarget(rowWidget);
                auto *row = new QHBoxLayout(rowWidget);
                row->setContentsMargins(0, 0, 0, 0);
                row->setSpacing(std::max(2, rowSpacing - 1));
                row->setAlignment(Qt::AlignVCenter);

                auto *accent = new QWidget(rowWidget);
                accent->setFixedWidth(accentWidth);
                accent->setFixedHeight(accentHeight);
                accent->setStyleSheet(
                    QString("background:%1; border-radius:%2px;")
                        .arg(color.name())
                        .arg(std::max(1, accentWidth / 2)));
                installSelectionTarget(accent);
                row->addWidget(accent, 0, Qt::AlignVCenter);

                auto *card = new QWidget(rowWidget);
                card->setObjectName("PredictionOutcomeRowCard");
                card->setFixedHeight(outcomeRowHeight);
                card->setCursor(broadcasterView ? Qt::ArrowCursor
                                                : Qt::PointingHandCursor);
                installSelectionTarget(card);
                auto *cardLayout = new QHBoxLayout(card);
                cardLayout->setContentsMargins(
                    sectionPad, std::max(1, int(1 * effectiveScale)),
                    sectionPad, std::max(1, int(1 * effectiveScale)));
                cardLayout->setSpacing(std::max(4, int(5 * effectiveScale)));

                auto *nameLabel = new QLabel(outcome.title, card);
                nameLabel->setObjectName("PredictionOutcomeName");
                nameLabel->setFont(buttonFont);
                nameLabel->setToolTip(outcome.title);
                nameLabel->setMinimumWidth(0);
                nameLabel->setSizePolicy(QSizePolicy::Ignored,
                                         QSizePolicy::Preferred);
                nameLabel->setStyleSheet(
                    QString("color:%1; background: transparent; border: none;")
                        .arg(color.name()));
                installSelectionTarget(nameLabel);
                cardLayout->addWidget(nameLabel, 1, Qt::AlignVCenter);

                auto *metaWidget = new QWidget(card);
                installSelectionTarget(metaWidget);
                auto *metaLayout = new QHBoxLayout(metaWidget);
                metaLayout->setContentsMargins(0, 0, 0, 0);
                metaLayout->setSpacing(std::max(3, int(4 * effectiveScale)));

                auto *returnValue = new QLabel(
                    QString::number(multiplier, 'f', 1) + "x", metaWidget);
                returnValue->setObjectName("PredictionReturn_" + outcome.id);
                returnValue->setFont(compactMetaFont);
                returnValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                installSelectionTarget(returnValue);
                metaLayout->addWidget(returnValue, 0, Qt::AlignVCenter);

                auto *returnLabel = new QLabel("RETURN", metaWidget);
                returnLabel->setObjectName("PredictionOutcomeStats");
                returnLabel->setFont(compactMetaFont);
                returnLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                installSelectionTarget(returnLabel);
                metaLayout->addWidget(returnLabel, 0, Qt::AlignVCenter);

                auto *pctLabel = new QLabel(QString::number(pct) + "%", card);
                pctLabel->setObjectName("PredictionPct_" + outcome.id);
                pctLabel->setFont(compactPctFont);
                pctLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                pctLabel->setStyleSheet(
                    QString("color:%1; background: transparent; border: none;")
                        .arg(color.name()));
                installSelectionTarget(pctLabel);
                metaLayout->addWidget(pctLabel, 0, Qt::AlignVCenter);

                cardLayout->addWidget(metaWidget, 0, Qt::AlignVCenter);

                row->addWidget(card, 1);
                outcomesLayout->addWidget(rowWidget);
            }

            this->outcomesScrollArea_->setWidget(outcomesWidget);
            QObject::connect(this->outcomesScrollArea_->verticalScrollBar(),
                             &QScrollBar::valueChanged, this,
                             [this](int value) {
                                 this->outcomesScrollValue_ = value;
                             });
            QTimer::singleShot(0, this, [this] {
                if (this->outcomesScrollArea_ != nullptr)
                {
                    this->outcomesScrollArea_->verticalScrollBar()->setValue(
                        this->outcomesScrollValue_);
                }
            });
            layout->addWidget(outcomesPanel);
        }
        else
        {
            const QColor selectedColor =
                outcomeColor(selectedOutcomeIndex, selectedOutcome->color);
            const int pct =
                totalPoints > 0
                    ? static_cast<int>((selectedOutcome->totalPoints * 100.0) /
                                       totalPoints)
                    : 0;
            const double multiplier = selectedOutcome->totalPoints > 0
                                          ? static_cast<double>(totalPoints) /
                                                selectedOutcome->totalPoints
                                          : 0.0;
            const auto focusedTitleFont = getApp()->getFonts()->getFont(
                FontStyle::UiMediumBold, effectiveScale * 1.26F);
            const auto focusedPctFont = getApp()->getFonts()->getFont(
                FontStyle::UiMediumBold, effectiveScale * 2.15F);
            const auto metaLabelFont = getApp()->getFonts()->getFont(
                FontStyle::UiMedium, effectiveScale * 0.82F);
            const auto metaValueFont = getApp()->getFonts()->getFont(
                FontStyle::UiMediumBold, effectiveScale * 1.02F);
            const int iconSize = std::max(10, int(12 * effectiveScale));

            auto *detailOuter = new QWidget(this->activeWidget_);
            detailOuter->setMinimumWidth(0);
            detailOuter->setSizePolicy(QSizePolicy::Ignored,
                                       QSizePolicy::Fixed);
            auto *detailOuterLayout = new QVBoxLayout(detailOuter);
            detailOuterLayout->setContentsMargins(0, 0, 0, 0);
            detailOuterLayout->setSpacing(0);

            auto *detailCard = new QWidget(detailOuter);
            detailCard->setObjectName("PredictionCard");
            detailCard->setMinimumWidth(0);
            detailCard->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            auto *detailLayout = new QVBoxLayout(detailCard);
            const int verticalCardPad = sectionPad;
            detailLayout->setContentsMargins(sectionPad * 2, verticalCardPad,
                                             sectionPad * 2, verticalCardPad);
            detailLayout->setSpacing(rowSpacing);

            auto *selectedTitle =
                new QLabel(selectedOutcome->title, detailCard);
            selectedTitle->setFont(focusedTitleFont);
            selectedTitle->setAlignment(Qt::AlignCenter);
            selectedTitle->setWordWrap(false);
            selectedTitle->setMinimumWidth(0);
            selectedTitle->setSizePolicy(QSizePolicy::Ignored,
                                         QSizePolicy::Fixed);
            selectedTitle->setToolTip(selectedOutcome->title);
            selectedTitle->setStyleSheet(
                QString("color:%1; font-weight: 700; background: transparent; "
                        "border: none;")
                    .arg(selectedColor.name()));
            detailLayout->addWidget(selectedTitle);

            {
                const int navIconSize = std::max(11, int(13 * effectiveScale));
                const int navBtnSize = std::max(18, int(22 * effectiveScale));

                QColor normalIconColor = selectedColor;
                normalIconColor.setAlpha(
                    std::min(255, std::max(160, selectedColor.alpha())));

                QColor hoverIconColor = selectedColor;
                hoverIconColor.setAlpha(255);

                QColor pressedIconColor = selectedColor.darker(110);
                pressedIconColor.setAlpha(255);

                QIcon navIcon;
                navIcon.addPixmap(renderSvgIcon(SVG_CHEVRON_LEFT,
                                                normalIconColor, navIconSize),
                                  QIcon::Normal, QIcon::Off);
                navIcon.addPixmap(renderSvgIcon(SVG_CHEVRON_LEFT,
                                                hoverIconColor, navIconSize),
                                  QIcon::Active, QIcon::Off);
                navIcon.addPixmap(renderSvgIcon(SVG_CHEVRON_LEFT,
                                                pressedIconColor, navIconSize),
                                  QIcon::Selected, QIcon::Off);

                auto *navButton = new QPushButton(detailCard);
                navButton->setObjectName("PredictionOutcomeBackIconButton");
                navButton->setFlat(true);
                navButton->setIcon(navIcon);
                navButton->setIconSize(QSize(navIconSize, navIconSize));
                navButton->setToolTip("Back to all options");
                navButton->setCursor(Qt::PointingHandCursor);
                navButton->setSizePolicy(QSizePolicy::Fixed,
                                         QSizePolicy::Fixed);
                navButton->setFixedSize(navBtnSize, navBtnSize);
                navButton->setStyleSheet("QPushButton {"
                                         "background: transparent;"
                                         "border: none;"
                                         "padding: 0px;"
                                         "margin: 0px;"
                                         "outline: none;"
                                         "}"
                                         "QPushButton:hover {"
                                         "background: transparent;"
                                         "border: none;"
                                         "}"
                                         "QPushButton:pressed {"
                                         "background: transparent;"
                                         "border: none;"
                                         "}");
                QObject::connect(navButton, &QPushButton::clicked, this,
                                 [this] {
                                     this->selectedBettingOutcomeId_.clear();
                                     this->updateUI();
                                 });

                // Reposition overlay button whenever the card lays out
                auto repositionNav = [navButton, selectedTitle, sectionPad] {
                    if (!selectedTitle->isVisible())
                    {
                        return;
                    }
                    int titleY = selectedTitle->y();
                    int titleH = selectedTitle->height();
                    int btnH = navButton->height();
                    navButton->move(sectionPad, titleY + (titleH - btnH) / 2);
                    navButton->raise();
                };
                QTimer::singleShot(0, detailCard, repositionNav);
                new ResizeWatcher(detailCard, repositionNav, detailCard);
            }

            auto *selectedPct =
                new QLabel(QString::number(pct) + "%", detailCard);
            selectedPct->setFont(focusedPctFont);
            selectedPct->setAlignment(Qt::AlignCenter);
            selectedPct->setStyleSheet(
                QString("color:%1; font-weight: 700; background: transparent; "
                        "border: none;")
                    .arg(selectedColor.name()));
            detailLayout->addWidget(selectedPct);

            auto *statsRow = new QHBoxLayout();
            statsRow->setSpacing(
                std::max(sectionSpacing, int(8 * effectiveScale)));

            auto addDetailStat = [&](const QString &svgData,
                                     const QString &label, const QString &value,
                                     const QString &tooltip = QString(),
                                     const QString &statRole = QString()) {
                auto *column = new QWidget(detailCard);
                column->setMinimumWidth(0);
                column->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
                auto *columnLayout = new QVBoxLayout(column);
                columnLayout->setContentsMargins(0, 0, 0, 0);
                columnLayout->setSpacing(
                    std::max(1, int(1.5 * effectiveScale)));

                auto *header = new QWidget(column);
                auto *headerLayout = new QHBoxLayout(header);
                headerLayout->setContentsMargins(0, 0, 0, 0);
                headerLayout->setSpacing(std::max(2, int(3 * effectiveScale)));

                auto *iconLabel = new QLabel(header);
                iconLabel->setPixmap(
                    renderSvgIcon(svgData, selectedColor, iconSize));
                iconLabel->setFixedSize(iconSize, iconSize);
                headerLayout->addWidget(iconLabel, 0, Qt::AlignCenter);

                auto *labelWidget = new QLabel(label, header);
                labelWidget->setObjectName("PredictionBetDetailLabel");
                labelWidget->setFont(metaLabelFont);
                headerLayout->addWidget(labelWidget, 0, Qt::AlignCenter);
                headerLayout->addStretch(1);
                columnLayout->addWidget(header, 0, Qt::AlignHCenter);

                auto *valueWidget = new QLabel(value, column);
                valueWidget->setObjectName("PredictionBetDetailValue");
                valueWidget->setFont(metaValueFont);
                valueWidget->setAlignment(Qt::AlignHCenter);
                valueWidget->setProperty("outcomeId", selectedOutcome->id);
                valueWidget->setProperty("statRole", statRole);
                if (!tooltip.isEmpty())
                {
                    valueWidget->setToolTip(tooltip);
                }
                columnLayout->addWidget(valueWidget, 0, Qt::AlignHCenter);

                statsRow->addWidget(column, 1);
            };

            addDetailStat(SVG_TROPHY, "PAYOUT",
                          QString::number(multiplier, 'f', 1) + "x", QString(),
                          "multiplier");
            addDetailStat(SVG_USERS, "VOTERS",
                          locale.toString(selectedOutcome->totalUsers),
                          QString(), "users");
            addDetailStat(SVG_POINTS, "POINTS",
                          formatCompact(selectedOutcome->totalPoints),
                          QString(), "points");
            addDetailStat(SVG_CROWN, "TOP BET",
                          formatCompact(selectedOutcome->topPoints),
                          selectedOutcome->topPredictorName, "crown");

            auto *statsWidget = new QWidget(detailCard);
            statsWidget->setObjectName("PredictionSelectedStats");
            statsWidget->setLayout(statsRow);
            statsWidget->setMinimumWidth(0);
            statsWidget->setSizePolicy(QSizePolicy::Ignored,
                                       QSizePolicy::Fixed);
            detailLayout->addWidget(statsWidget);
            detailLayout->activate();
            const int detailHeight = detailLayout->sizeHint().height();
            detailCard->setMinimumHeight(detailHeight);
            detailCard->setMaximumHeight(detailHeight);
            detailOuterLayout->addWidget(detailCard);
            detailOuterLayout->activate();
            const int outerHeight = detailOuterLayout->sizeHint().height();
            detailOuter->setMinimumHeight(outerHeight);
            detailOuter->setMaximumHeight(outerHeight);
            layout->addWidget(detailOuter);
        }
    }

    // ── Wager bottom bar (Active only) ─────────────────────
    if (!broadcasterView && prediction.status == "ACTIVE" &&
        (!multiOutcomeBetting || selectedOutcome != nullptr))
    {
        const qint64 balance = this->channel_->channelPointBalance();
        const int maxBet = int(std::max<qint64>(
            0, std::min<qint64>(balance > 0 ? balance : 250000, 250000)));

        this->bottomWidget_ = new QWidget();
        this->bottomWidget_->setObjectName("PredictionCard");
        this->bottomWidget_->setFont(uiFont);
        this->bottomWidget_->setSizePolicy(QSizePolicy::Preferred,
                                           QSizePolicy::Fixed);
        auto *bottomLayout = new QVBoxLayout(this->bottomWidget_);
        bottomLayout->setContentsMargins(sectionPad * 2, sectionPad * 1.5,
                                         sectionPad * 2, sectionPad * 1.5);
        bottomLayout->setSpacing(sectionSpacing);

        // ── Wager + Balance header ──
        {
            auto *wagerRow = new QHBoxLayout();
            wagerRow->setSpacing(rowSpacing);

            auto *wagerLabel = new QLabel("Wager", this->bottomWidget_);
            wagerLabel->setObjectName("PredictionSectionTitle");
            wagerLabel->setFont(buttonFont);
            wagerRow->addWidget(wagerLabel);
            wagerRow->addStretch(1);

            auto *balLabel = new QLabel(
                QString("Bal: %1").arg(
                    balance >= 0 ? formatChannelPoints(balance) : "..."),
                this->bottomWidget_);
            balLabel->setObjectName("PredictionBalanceLabel");
            balLabel->setFont(uiFont);
            wagerRow->addWidget(balLabel);
            bottomLayout->addLayout(wagerRow);
        }

        // ── Amount input + quick % buttons ──
        QLineEdit *amountInput = nullptr;
        {
            auto *inputRow = new QHBoxLayout();
            inputRow->setSpacing(rowSpacing);

            amountInput = new QLineEdit(this->bottomWidget_);
            amountInput->setObjectName("PredictionWagerInput");
            amountInput->setFont(uiFont);
            amountInput->setFixedHeight(compactControlHeight);
            amountInput->setPlaceholderText("Amount");
            amountInput->setValidator(
                new QIntValidator(1, maxBet, amountInput));
            if (this->bettingWagerAmount_ > 0)
            {
                amountInput->setText(
                    QString::number(this->bettingWagerAmount_));
            }
            QObject::connect(amountInput, &QLineEdit::textChanged, this,
                             [this](const QString &text) {
                                 bool ok;
                                 int val = text.toInt(&ok);
                                 this->bettingWagerAmount_ = ok ? val : 0;
                             });
            inputRow->addWidget(amountInput, 1);

            struct QuickPct {
                const char *label;
                int pct;
            };
            const QuickPct quickPcts[] = {
                {"10%", 10}, {"25%", 25}, {"50%", 50}};
            for (const auto &qp : quickPcts)
            {
                auto *btn = new QPushButton(qp.label, this->bottomWidget_);
                btn->setObjectName("PredictionWagerQuickButton");
                btn->setFont(statFont);
                btn->setFixedHeight(compactControlHeight);
                const int pctVal = qp.pct;
                QPointer<QLineEdit> inputPtr = amountInput;
                QObject::connect(btn, &QPushButton::clicked, this,
                                 [inputPtr, maxBet, pctVal, this] {
                                     if (!inputPtr)
                                     {
                                         return;
                                     }
                                     int amount =
                                         std::max(1, maxBet * pctVal / 100);
                                     inputPtr->setText(QString::number(amount));
                                     this->bettingWagerAmount_ = amount;
                                 });
                inputRow->addWidget(btn);
            }

            auto *maxBtn = new QPushButton("MAX", this->bottomWidget_);
            maxBtn->setObjectName("PredictionWagerQuickButton");
            maxBtn->setFont(statFont);
            maxBtn->setFixedHeight(compactControlHeight);
            QPointer<QLineEdit> maxInputPtr = amountInput;
            QObject::connect(maxBtn, &QPushButton::clicked, this,
                             [maxInputPtr, maxBet, this] {
                                 if (!maxInputPtr)
                                 {
                                     return;
                                 }
                                 maxInputPtr->setText(QString::number(maxBet));
                                 this->bettingWagerAmount_ = maxBet;
                             });
            inputRow->addWidget(maxBtn);
            bottomLayout->addLayout(inputRow);
        }

        // ── Vote buttons ──
        {
            auto *voteRow = new QHBoxLayout();
            voteRow->setSpacing(sectionSpacing);

            const auto eventId = prediction.id;
            QString authError;
            const auto auth =
                MoltorinoAuth::resolveCurrentUserToken(&authError);

            if (!auth.hasToken())
            {
                auto *noAuthLabel = new QLabel(
                    authError.isEmpty() ? moltorinoAuthRequiredMessage(
                                              "placing prediction bets")
                                        : authError,
                    this->bottomWidget_);
                noAuthLabel->setObjectName("PredictionInfoLabel");
                noAuthLabel->setWordWrap(true);
                noAuthLabel->setAlignment(Qt::AlignCenter);
                noAuthLabel->setFont(uiFont);
                bottomLayout->addWidget(noAuthLabel);
            }
            else if (!multiOutcomeBetting)
            {
                for (int i = 0;
                     i < static_cast<int>(prediction.outcomes.size()); ++i)
                {
                    const auto &outcome = prediction.outcomes.at(i);
                    const auto outcomeId = outcome.id;
                    const QColor btnColor = outcomeColor(i, outcome.color);

                    auto *voteBtn =
                        new QPushButton("Vote", this->bottomWidget_);
                    voteBtn->setObjectName("PredictionVoteButtonDynamic");
                    voteBtn->setFont(buttonFont);
                    voteBtn->setFixedHeight(compactControlHeight);
                    voteBtn->setSizePolicy(QSizePolicy::Expanding,
                                           QSizePolicy::Fixed);

                    const int voteRadius = std::max(1, int(2 * rawScale));
                    const int votePaddingY = 0;
                    const int votePaddingX =
                        std::max(4, int(5 * effectiveScale));
                    const int voteMinHeight =
                        std::max(14, int(20 * effectiveScale));
                    voteBtn->setStyleSheet(
                        QString("QPushButton {"
                                "background: %1;"
                                "color: white;"
                                "border: 1px solid transparent;"
                                "border-radius: %2px;"
                                "font-weight: 600;"
                                "padding: %3px %4px;"
                                "min-height: %5px;"
                                "}"
                                "QPushButton:hover {"
                                "background: %6;"
                                "}"
                                "QPushButton:disabled {"
                                "background: %7;"
                                "color: rgba(255,255,255,0.7);"
                                "}")
                            .arg(btnColor.name())
                            .arg(voteRadius)
                            .arg(votePaddingY)
                            .arg(votePaddingX)
                            .arg(voteMinHeight)
                            .arg(btnColor.lighter(112).name())
                            .arg(btnColor.darker(112).name()));

                    QObject::connect(
                        voteBtn, &QPushButton::clicked, this,
                        [this, eventId, outcomeId, voteBtn,
                         authToken = auth.token] {
                            const int points = this->bettingWagerAmount_;
                            if (points <= 0)
                            {
                                this->channel_->addSystemMessage(
                                    "Enter a wager amount first.");
                                return;
                            }

                            voteBtn->setEnabled(false);
                            voteBtn->setText("Betting...");

                            QPointer<PredictionDialog> self = this;
                            QPointer<QPushButton> btn = voteBtn;
                            TwitchGql::makePrediction(
                                eventId, outcomeId, points, authToken,
                                [self, points, outcomeId] {
                                    if (!self)
                                    {
                                        return;
                                    }

                                    QString outcomeTitle = "prediction";
                                    if (self->currentPrediction_)
                                    {
                                        self->currentPrediction_->selfPoints =
                                            points;
                                        self->currentPrediction_
                                            ->selfOutcomeId = outcomeId;
                                        for (const auto &o :
                                             self->currentPrediction_->outcomes)
                                        {
                                            if (o.id == outcomeId)
                                            {
                                                outcomeTitle =
                                                    QString("\"%1\"").arg(
                                                        o.title);
                                                break;
                                            }
                                        }
                                    }
                                    // Update channel memory locally and broadcast.
                                    // Copy the prediction out before calling
                                    // setActivePrediction(), otherwise we'd
                                    // try to upgrade a shared lock to a
                                    // unique lock on the same mutex and
                                    // deadlock the GUI thread.
                                    if (self->channel_)
                                    {
                                        std::optional<
                                            TwitchChannel::PredictionEvent>
                                            mutatedPrediction;
                                        {
                                            auto guard =
                                                self->channel_
                                                    ->accessPrediction();
                                            if (guard->has_value())
                                            {
                                                mutatedPrediction =
                                                    guard->value();
                                            }
                                        }

                                        if (mutatedPrediction.has_value())
                                        {
                                            mutatedPrediction->selfPoints =
                                                points;
                                            mutatedPrediction->selfOutcomeId =
                                                outcomeId;
                                            self->channel_->setActivePrediction(
                                                std::move(*mutatedPrediction));
                                        }
                                    }

                                    /*
                                        self->channel_->addSystemMessage(
                                            QString("Placed %1 points on %2 in prediction.")
                                                .arg(QLocale().toString(points),
                                                     outcomeTitle));
                                        */

                                    if (getSettings()
                                            ->predictionAutoCloseDialog)
                                    {
                                        self->close();
                                    }
                                    else
                                    {
                                        self->updateUI();
                                    }
                                },
                                [self, btn](const QString &error) {
                                    if (!self)
                                    {
                                        return;
                                    }
                                    self->channel_->addSystemMessage(
                                        "Failed to place bet: " +
                                        normalizeMoltorinoAuthError(
                                            "placing prediction bets", error));
                                    if (btn)
                                    {
                                        btn->setEnabled(true);
                                        btn->setText("Vote");
                                    }
                                });
                        });

                    voteRow->addWidget(voteBtn, 1);
                }
            }
            else if (selectedOutcome != nullptr)
            {
                const QColor selectedColor =
                    outcomeColor(selectedOutcomeIndex, selectedOutcome->color);
                const int voteRadius = std::max(1, int(2 * rawScale));
                const int votePaddingY = 0;
                const int votePaddingX = std::max(4, int(5 * effectiveScale));
                const int voteMinHeight =
                    std::max(14, int(20 * effectiveScale));
                const auto selectedOutcomeId = selectedOutcome->id;
                const auto selectedOutcomeTitle = selectedOutcome->title;
                auto *voteBtn = new QPushButton("Vote", this->bottomWidget_);
                voteBtn->setFont(buttonFont);
                voteBtn->setFixedHeight(compactControlHeight);
                voteBtn->setSizePolicy(QSizePolicy::Expanding,
                                       QSizePolicy::Fixed);
                voteBtn->setStyleSheet(
                    QString("QPushButton {"
                            "background: %1;"
                            "color: white;"
                            "border: 1px solid transparent;"
                            "border-radius: %2px;"
                            "font-weight: 600;"
                            "padding: %3px %4px;"
                            "min-height: %5px;"
                            "}"
                            "QPushButton:hover {"
                            "background: %6;"
                            "}"
                            "QPushButton:disabled {"
                            "background: %7;"
                            "color: rgba(255,255,255,0.7);"
                            "}")
                        .arg(selectedColor.name())
                        .arg(voteRadius)
                        .arg(votePaddingY)
                        .arg(votePaddingX)
                        .arg(voteMinHeight)
                        .arg(selectedColor.lighter(112).name())
                        .arg(selectedColor.darker(112).name()));

                QObject::connect(
                    voteBtn, &QPushButton::clicked, this,
                    [this, eventId, voteBtn, authToken = auth.token,
                     selectedOutcomeId, selectedOutcomeTitle] {
                        const int points = this->bettingWagerAmount_;
                        if (points <= 0)
                        {
                            this->channel_->addSystemMessage(
                                "Enter a wager amount first.");
                            return;
                        }

                        voteBtn->setEnabled(false);
                        voteBtn->setText("Betting...");

                        QPointer<PredictionDialog> self = this;
                        QPointer<QPushButton> btn = voteBtn;
                        TwitchGql::makePrediction(
                            eventId, selectedOutcomeId, points, authToken,
                            [self, points, selectedOutcomeTitle,
                             selectedOutcomeId] {
                                if (!self)
                                {
                                    return;
                                }
                                if (self->currentPrediction_)
                                {
                                    self->currentPrediction_->selfPoints =
                                        points;
                                    self->currentPrediction_->selfOutcomeId =
                                        selectedOutcomeId;
                                }
                                // Update channel memory locally and broadcast.
                                // Copy out of the shared guard before writing
                                // back to avoid deadlocking on the prediction
                                // mutex.
                                if (self->channel_)
                                {
                                    std::optional<
                                        TwitchChannel::PredictionEvent>
                                        mutatedPrediction;
                                    {
                                        auto guard =
                                            self->channel_->accessPrediction();
                                        if (guard->has_value())
                                        {
                                            mutatedPrediction = guard->value();
                                        }
                                    }

                                    if (mutatedPrediction.has_value())
                                    {
                                        mutatedPrediction->selfPoints = points;
                                        mutatedPrediction->selfOutcomeId =
                                            selectedOutcomeId;
                                        self->channel_->setActivePrediction(
                                            std::move(*mutatedPrediction));
                                    }
                                }

                                /*
                                self->channel_->addSystemMessage(
                                    QString("Placed %1 points on \"%2\" in prediction.")
                                        .arg(QLocale().toString(points),
                                             selectedOutcomeTitle));
                                */
                                if (getSettings()->predictionAutoCloseDialog)
                                {
                                    self->close();
                                }
                                else
                                {
                                    self->updateUI();
                                }
                            },
                            [self, btn](const QString &error) {
                                if (!self)
                                {
                                    return;
                                }
                                self->channel_->addSystemMessage(
                                    "Failed to place bet: " +
                                    normalizeMoltorinoAuthError(
                                        "placing prediction bets", error));
                                if (btn)
                                {
                                    btn->setEnabled(true);
                                    btn->setText("Vote");
                                }
                            });
                    });

                voteRow->addWidget(voteBtn, 1);
            }

            bottomLayout->addLayout(voteRow);
        }

        this->mainLayout_->addWidget(this->bottomWidget_);
        this->bottomWidget_->show();
    }
    else
    {
    }
}

void PredictionDialog::refreshStyle()
{
    const float rawScale = this->scale();
    const float effectiveScale = contentScale(rawScale);
    const int radius = std::max(1, int(2 * rawScale));
    const int smallRadius = std::max(1, int(2 * rawScale));
    const int inputPaddingY = 0;
    const int inputPaddingX = std::max(4, int(5 * effectiveScale));
    const int inputMinHeight = std::max(14, int(20 * effectiveScale));
    const int controlPaddingY = std::max(1, int(2 * effectiveScale));
    const int controlPaddingX = std::max(4, int(6 * effectiveScale));
    const int controlMinHeight = std::max(16, int(24 * effectiveScale));
    const int compactControlPaddingY = 0;
    const int compactControlPaddingX = std::max(4, int(5 * effectiveScale));
    const int compactControlMinHeight = std::max(14, int(20 * effectiveScale));
    const int scrollbarWidth = std::max(3, int(4 * effectiveScale));
    const int scrollbarRadius = std::max(1, int(2 * effectiveScale));
    const int scrollbarMinHeight = std::max(12, int(16 * effectiveScale));

    auto windowBackground = this->theme->window.background;
    auto textColor = this->theme->window.text;
    auto mutedColor = textColor;
    mutedColor.setAlpha(160);
    auto borderColor = this->theme->splits.header.border;
    auto cardBackground = this->theme->splits.header.background;
    auto inputBackground = this->theme->splits.input.background;
    auto accentColor = this->theme->tabs.selected.backgrounds.regular;
    auto accentTextColor = this->theme->tabs.selected.text;
    auto dangerColor = QColor("#d14343");

    this->closeButton_->setColor(textColor);
    this->headerTitleLabel_->setFont(
        getApp()->getFonts()->getFont(FontStyle::UiMediumBold, effectiveScale));
    this->headerSubtitleLabel_->setFont(
        getApp()->getFonts()->getFont(FontStyle::UiMedium, effectiveScale));
    this->headerSubtitleLabel_->setStyleSheet(QString());

    this->modToggleButton_->setFont(
        getApp()->getFonts()->getFont(FontStyle::UiMedium, effectiveScale));

    this->headerWidget_->layout()->setContentsMargins(
        std::max(2, int(4 * rawScale)), std::max(2, int(3 * rawScale)),
        std::max(2, int(4 * rawScale)), std::max(2, int(3 * rawScale)));
    this->headerWidget_->layout()->setSpacing(std::max(2, int(3 * rawScale)));

    this->mainLayout_->setContentsMargins(
        std::max(3, int(5 * rawScale)), std::max(3, int(5 * rawScale)),
        std::max(3, int(5 * rawScale)), std::max(3, int(5 * rawScale)));
    this->mainLayout_->setSpacing(0);
    if (auto *separator =
            this->findChild<QWidget *>("PredictionDialogSeparator"))
    {
        separator->setFixedHeight(scaledSeparatorHeight(rawScale));
    }

    this->setStyleSheet(
        QString(R"(
            QWidget#PredictionDialogRoot {
                background: %1;
            }
            QWidget#PredictionCard {
                background: %2;
                border: 1px solid %3;
                border-radius: %4px;
            }
            QScrollArea#PredictionScrollArea,
            QScrollArea#PredictionOutcomesScrollArea,
            QWidget#PredictionDialogContent,
            QWidget#PredictionOutcomesContent,
            QWidget#PredictionBottomBar {
                border: none;
                background: transparent;
            }
            QWidget#PredictionOutcomesPanel {
                background: %2;
                border: 1px solid %3;
                border-radius: %4px;
            }
            QWidget#PredictionOutcomeRowCard {
                background: %7;
                border: 1px solid %3;
                border-radius: %8px;
            }
            QWidget#PredictionOutcomeRowCard[selectedManageOutcome="true"] {
                border: 1px solid %19;
            }
            QLabel#PredictionHeaderTitle {
                color: %5;
                font-weight: 700;
            }
            QLabel#PredictionCurrentTitle {
                color: %5;
                font-weight: 700;
            }
            QLabel#PredictionSectionTitle {
                color: %6;
                font-weight: 600;
            }
            QLabel#PredictionOutcomeName {
                color: %5;
                font-weight: 600;
            }
            QLabel#PredictionHeaderSubtitle,
            QLabel#PredictionCountLabel,
            QLabel#PredictionBalanceLabel,
            QLabel#PredictionInfoLabel,
            QLabel#PredictionOutcomeStats {
                color: %6;
            }
            QLineEdit#PredictionTitleInput,
            QLineEdit#PredictionOptionInput {
                background: %7;
                color: %5;
                border: 1px solid %3;
                border-radius: %8px;
                padding: %9px %10px;
                min-height: %11px;
            }
            QWidget#PredictionTitlePicker {
                background: %7;
                color: %5;
                border: 1px solid %3;
                border-radius: %8px;
                padding: 0;
                min-height: %11px;
            }
            QWidget#PredictionTitlePicker QLineEdit#PredictionTitleInput {
                background: transparent;
                color: %5;
                border: none;
                padding: %9px %10px;
                min-height: %11px;
            }
            QToolButton#PredictionTitlePickerButton {
                background: transparent;
                color: %6;
                border: none;
                padding: 0;
            }
            QListWidget#PredictionTitleTemplatePopup {
                background: %1;
                color: %5;
                border: 1px solid %3;
                outline: none;
            }
            QListWidget#PredictionTitleTemplatePopup::viewport {
                background: %1;
            }
            QListWidget#PredictionTitleTemplatePopup::item {
                background: %1;
                padding: %9px %10px;
            }
            QListWidget#PredictionTitleTemplatePopup::item:hover {
                background: %7;
                color: %5;
            }
            QListWidget#PredictionTitleTemplatePopup::item:selected {
                background: %7;
                color: %5;
            }
            QComboBox#PredictionDurationCombo,
            QComboBox#PredictionResolveCombo,
            QSpinBox#PredictionPointsInput {
                background: %7;
                color: %5;
                border: 1px solid %3;
                border-radius: %8px;
                padding: %12px %13px;
                min-height: %14px;
            }
            QComboBox#PredictionCreateDurationCombo {
                background: %7;
                color: %5;
                border: 1px solid %3;
                border-radius: %8px;
                padding: %15px %16px;
                min-height: %17px;
            }
            QComboBox#PredictionManageResolveCombo {
                background: %7;
                color: %5;
                border: 1px solid %3;
                border-radius: %8px;
                padding: %15px %16px;
                min-height: %17px;
            }
            QComboBox#PredictionDurationCombo QAbstractItemView,
            QComboBox#PredictionResolveCombo QAbstractItemView,
            QComboBox#PredictionCreateDurationCombo QAbstractItemView,
            QComboBox#PredictionManageResolveCombo QAbstractItemView {
                background: %1;
                color: %5;
                border: 1px solid %3;
                outline: none;
                selection-background-color: %7;
                selection-color: %5;
            }
            QPushButton#PredictionCreateAddOptionButton,
            QPushButton#PredictionCreateRemoveOptionButton,
            QPushButton#PredictionCreateStartButton {
                border: 1px solid transparent;
                border-radius: %8px;
                font-weight: 600;
                padding: %15px %16px;
                min-height: %17px;
            }
            QPushButton#PredictionCreateAddOptionButton,
            QPushButton#PredictionCreateRemoveOptionButton {
                background: %18;
                color: %5;
                border: 1px solid %3;
            }
            QPushButton#PredictionCreateStartButton {
                background: %19;
                color: %20;
            }
            QPushButton#PredictionManagePrimaryButton,
            QPushButton#PredictionManageDangerButton {
                border: 1px solid transparent;
                border-radius: %8px;
                font-weight: 600;
                padding: %15px %16px;
                min-height: %17px;
            }
            QPushButton#PredictionManagePrimaryButton {
                background: %19;
                color: %20;
            }
            QPushButton#PredictionManageDangerButton {
                background: %21;
                color: white;
            }
            QPushButton#PredictionAddOptionButton,
            QPushButton#PredictionSecondaryButton,
            QPushButton#PredictionPrimaryButton,
            QPushButton#PredictionDangerButton {
                border: 1px solid transparent;
                border-radius: %8px;
                font-weight: 600;
                padding: %12px %13px;
                min-height: %14px;
            }
            QPushButton#PredictionAddOptionButton,
            QPushButton#PredictionSecondaryButton {
                background: %18;
                color: %5;
                border: 1px solid %3;
            }
            QPushButton#PredictionPrimaryButton {
                background: %19;
                color: %20;
            }
            QPushButton#PredictionOutcomeChoiceButton {
                background: %18;
                color: %5;
                border: 1px solid %3;
                border-radius: %8px;
                font-weight: 600;
                padding: %9px %10px;
                min-height: %11px;
                text-align: left;
            }
            QPushButton#PredictionOutcomeChoiceButton:hover {
                background: %7;
                color: %19;
            }
            QPushButton#PredictionDangerButton {
                background: %21;
                color: white;
            }
            QWidget#PredictionQuestionCard {
                background: %2;
                border: 1px solid %3;
                border-radius: %4px;
            }
            QLabel#PredictionCountdown {
                color: %6;
            }
            QLabel#PredictionBetDetailLabel {
                color: %6;
                font-weight: 600;
            }
            QLabel#PredictionBetDetailValue {
                color: %5;
                font-weight: 700;
            }
            QLabel#PredictionBetStatValue {
                color: %6;
            }
            QWidget#PredictionDivider {
                background: %3;
            }
            QLineEdit#PredictionWagerInput {
                background: %7;
                color: %5;
                border: 1px solid %3;
                border-radius: %8px;
                padding: %9px %10px;
                min-height: %11px;
            }
            QPushButton#PredictionWagerQuickButton {
                background: %18;
                color: %5;
                border: 1px solid %3;
                border-radius: %8px;
                font-weight: 600;
                padding: %15px %16px;
                min-height: %17px;
            }
            QScrollArea#PredictionOutcomesScrollArea QScrollBar:vertical {
                background: transparent;
                width: %22px;
                margin: 0;
            }
            QScrollArea#PredictionOutcomesScrollArea QScrollBar::handle:vertical {
                background: %6;
                border-radius: %23px;
                min-height: %24px;
            }
            QScrollArea#PredictionOutcomesScrollArea QScrollBar::add-line:vertical,
            QScrollArea#PredictionOutcomesScrollArea QScrollBar::sub-line:vertical,
            QScrollArea#PredictionOutcomesScrollArea QScrollBar::add-page:vertical,
            QScrollArea#PredictionOutcomesScrollArea QScrollBar::sub-page:vertical {
                background: transparent;
                height: 0;
            }
            QPushButton#PredictionModToggleButton {
                background: %18;
                color: %5;
                border: 1px solid %3;
                border-radius: %8px;
                font-weight: 600;
                padding: %15px %16px;
                min-height: %17px;
            }
            QPushButton#PredictionModToggleButton:hover {
                background: %7;
                color: %19;
            }
            QPushButton:disabled {
                color: %6;
            }
        )")
            .arg(windowBackground.name())           // %1
            .arg(cardBackground.name())             // %2
            .arg(borderColor.name())                // %3
            .arg(radius)                            // %4
            .arg(textColor.name())                  // %5
            .arg(mutedColor.name(QColor::HexArgb))  // %6
            .arg(inputBackground.name())            // %7
            .arg(smallRadius)                       // %8
            .arg(inputPaddingY)                     // %9
            .arg(inputPaddingX)                     // %10
            .arg(inputMinHeight)                    // %11
            .arg(controlPaddingY)                   // %12
            .arg(controlPaddingX)                   // %13
            .arg(controlMinHeight)                  // %14
            .arg(compactControlPaddingY)            // %15
            .arg(compactControlPaddingX)            // %16
            .arg(compactControlMinHeight)           // %17
            .arg(this->theme->isLightTheme()
                     ? cardBackground.darker(104).name()
                     : cardBackground.lighter(108).name())  // %18
            .arg(accentColor.name())                        // %19
            .arg(accentTextColor.name())                    // %20
            .arg(dangerColor.name())                        // %21
            .arg(scrollbarWidth)                            // %22
            .arg(scrollbarRadius)                           // %23
            .arg(scrollbarMinHeight));                      // %24
}

}  // namespace chatterino

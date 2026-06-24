#include "providers/moltorino/MoltorinoFeatureFlags.hpp"

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS

#    include "widgets/dialogs/ChannelPointsChartView.hpp"

#    include "controllers/channelpoints/ChannelPointsChartStore.hpp"
#    include "singletons/Theme.hpp"
#    include "util/Helpers.hpp"

#    include <QAreaSeries>
#    include <QChart>
#    include <QChartView>
#    include <QComboBox>
#    include <QDateTimeAxis>
#    include <QEvent>
#    include <QGraphicsEllipseItem>
#    include <QGraphicsLineItem>
#    include <QGraphicsScene>
#    include <QHBoxLayout>
#    include <QLabel>
#    include <QLinearGradient>
#    include <QLineSeries>
#    include <QListView>
#    include <QMouseEvent>
#    include <QPen>
#    include <QPushButton>
#    include <QTimer>
#    include <QVBoxLayout>
#    include <QValueAxis>

#    include <algorithm>
#    include <limits>
#    include <optional>

QT_USE_NAMESPACE

namespace {

using namespace chatterino;

constexpr QColor ACCENT_COLOR(0xE8, 0x92, 0x7C);

enum class ChartTimeRange {
    All,
    Last24Hours,
    Last7Days,
    Last14Days,
    Last30Days,
    Last90Days,
};

constexpr ChartTimeRange DEFAULT_CHART_TIME_RANGE = ChartTimeRange::Last14Days;

struct ChartTimeRangeOption {
    const char *label;
    ChartTimeRange range;
};

constexpr ChartTimeRangeOption CHART_TIME_RANGE_OPTIONS[] = {
    {"All time", ChartTimeRange::All},
    {"Last 24 hours", ChartTimeRange::Last24Hours},
    {"Last 7 days", ChartTimeRange::Last7Days},
    {"Last 14 days", ChartTimeRange::Last14Days},
    {"Last 30 days", ChartTimeRange::Last30Days},
    {"Last 90 days", ChartTimeRange::Last90Days},
};

void populateRangeCombo(QComboBox *combo)
{
    for (const auto &option : CHART_TIME_RANGE_OPTIONS)
    {
        combo->addItem(QString::fromUtf8(option.label),
                       static_cast<int>(option.range));
    }

    const int defaultIndex =
        combo->findData(static_cast<int>(DEFAULT_CHART_TIME_RANGE));
    combo->setCurrentIndex(defaultIndex >= 0 ? defaultIndex : 0);
}

ChartTimeRange chartTimeRangeFromCombo(const QComboBox *combo, int index)
{
    if (combo == nullptr || index < 0)
    {
        return DEFAULT_CHART_TIME_RANGE;
    }

    return static_cast<ChartTimeRange>(combo->itemData(index).toInt());
}

void setComboToChartTimeRange(QComboBox *combo, ChartTimeRange range)
{
    if (combo == nullptr)
    {
        return;
    }

    const int index = combo->findData(static_cast<int>(range));
    if (index >= 0)
    {
        combo->setCurrentIndex(index);
    }
}

QString utcAxisFormat(qint64 spanSeconds)
{
    if (spanSeconds <= 6 * 3600)
    {
        return QStringLiteral("HH:mm:ss");
    }
    if (spanSeconds <= 48 * 3600)
    {
        return QStringLiteral("HH:mm");
    }
    return QStringLiteral("MMM d HH:mm");
}

QString fullBalanceText(qint64 balance)
{
    return QLocale().toString(balance) + QStringLiteral(" points");
}

QString hoverTimeText(const QDateTime &time)
{
    return time.toUTC().toString(QStringLiteral("dd MMM yyyy HH:mm:ss")) +
           QStringLiteral(" UTC");
}

std::optional<QDateTime> rangeStartForPreset(ChartTimeRange range,
                                             const QDateTime &end)
{
    switch (range)
    {
        case ChartTimeRange::All:
            return std::nullopt;
        case ChartTimeRange::Last24Hours:
            return end.addSecs(-24 * 3600);
        case ChartTimeRange::Last7Days:
            return end.addDays(-7);
        case ChartTimeRange::Last14Days:
            return end.addDays(-14);
        case ChartTimeRange::Last30Days:
            return end.addDays(-30);
        case ChartTimeRange::Last90Days:
            return end.addDays(-90);
    }

    return std::nullopt;
}

QVector<ChannelPointsChartSample> samplesForRange(
    const QVector<ChannelPointsChartSample> &all, ChartTimeRange range)
{
    if (all.isEmpty() || range == ChartTimeRange::All)
    {
        return all;
    }

    const auto end = all.last().time;
    const auto start = rangeStartForPreset(range, end);
    if (!start.has_value())
    {
        return all;
    }

    QVector<ChannelPointsChartSample> result;
    result.reserve(all.size());

    for (const auto &sample : all)
    {
        if (sample.time >= *start && sample.time <= end)
        {
            result.append(sample);
        }
    }

    if (!result.isEmpty() && result.first().time > *start)
    {
        const auto it = std::find_if(all.rbegin(), all.rend(),
                                     [&](const auto &sample) {
                                         return sample.time < *start;
                                     });
        if (it != all.rend())
        {
            result.prepend(*it);
        }
    }

    return result;
}

int nearestSampleIndex(const QVector<ChannelPointsChartSample> &samples,
                       const QDateTime &time)
{
    if (samples.isEmpty())
    {
        return -1;
    }

    const auto ms = time.toMSecsSinceEpoch();
    int bestIndex = 0;
    qint64 bestDistance = std::numeric_limits<qint64>::max();

    for (int i = 0; i < samples.size(); ++i)
    {
        const auto distance =
            std::llabs(samples[i].time.toMSecsSinceEpoch() - ms);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestIndex = i;
        }
    }

    return bestIndex;
}

}  // namespace

namespace chatterino {

class ChannelPointsChartView::Private
{
public:
    QVector<ChannelPointsChartSample> allSamples;
    QVector<ChannelPointsChartSample> samples;
    ChartTimeRange selectedRange = DEFAULT_CHART_TIME_RANGE;

    QWidget *toolbarWidget = nullptr;
    QComboBox *rangeCombo = nullptr;
    QPushButton *resetButton = nullptr;
    QLabel *emptyLabel = nullptr;
    QLabel *calloutLabel = nullptr;
    QChart *chart = nullptr;
    QChartView *chartView = nullptr;
    QAreaSeries *areaSeries = nullptr;
    QLineSeries *lineSeries = nullptr;
    QDateTimeAxis *axisX = nullptr;
    QValueAxis *axisY = nullptr;
    QGraphicsLineItem *crosshair = nullptr;
    QGraphicsEllipseItem *highlightDot = nullptr;
    int hoveredIndex = -1;
    QColor accentColor = ACCENT_COLOR;

    void clearHover()
    {
        this->hoveredIndex = -1;
        if (this->crosshair != nullptr)
        {
            this->crosshair->hide();
        }
        if (this->highlightDot != nullptr)
        {
            this->highlightDot->hide();
        }
        if (this->calloutLabel != nullptr)
        {
            this->calloutLabel->hide();
        }
    }

    void updateToolbarVisibility()
    {
        const bool hasAnyData = !this->allSamples.isEmpty();
        if (this->toolbarWidget != nullptr)
        {
            this->toolbarWidget->setVisible(hasAnyData);
        }
    }

    void updateEmptyState()
    {
        const bool hasAnyData = !this->allSamples.isEmpty();
        const bool hasVisibleData = !this->samples.isEmpty();

        if (!hasAnyData)
        {
            this->emptyLabel->setText(
                QStringLiteral("No chart data yet for this channel."));
        }
        else if (!hasVisibleData)
        {
            this->emptyLabel->setText(
                QStringLiteral("No data in the selected time range."));
        }

        this->emptyLabel->setVisible(!hasVisibleData);
        this->chartView->setVisible(hasVisibleData);
        this->updateToolbarVisibility();
    }

    void updateResetButton()
    {
        if (this->resetButton == nullptr)
        {
            return;
        }

        const bool canReset =
            this->selectedRange != DEFAULT_CHART_TIME_RANGE ||
            (this->chart != nullptr && this->chart->isZoomed());
        this->resetButton->setEnabled(canReset);
    }

    void applyTimeRangeFilter()
    {
        this->samples = samplesForRange(this->allSamples, this->selectedRange);
        if (this->chart != nullptr)
        {
            this->chart->zoomReset();
        }
        this->rebuildChart();
        this->updateResetButton();
    }

    void rebuildChart()
    {
        if (this->chart == nullptr)
        {
            return;
        }

        this->chart->removeAllSeries();
        this->areaSeries = nullptr;
        this->lineSeries = nullptr;
        this->clearHover();
        this->updateEmptyState();

        if (this->samples.isEmpty())
        {
            return;
        }

        auto *fillLine = new QLineSeries(this->chart);
        this->lineSeries = new QLineSeries(this->chart);
        for (const auto &sample : this->samples)
        {
            const auto x = sample.time.toMSecsSinceEpoch();
            const auto y = static_cast<double>(sample.balance);
            fillLine->append(x, y);
            this->lineSeries->append(x, y);
        }

        auto *baseline = new QLineSeries(this->chart);
        const auto minTime = this->samples.first().time.toMSecsSinceEpoch();
        const auto maxTime = this->samples.last().time.toMSecsSinceEpoch();
        const auto minBalance = std::min_element(
            this->samples.begin(), this->samples.end(),
            [](const auto &left, const auto &right) {
                return left.balance < right.balance;
            })->balance;
        baseline->append(minTime, minBalance);
        baseline->append(maxTime, minBalance);

        this->areaSeries = new QAreaSeries(fillLine, baseline);

        QLinearGradient gradient(QPointF(0, 0), QPointF(0, 1));
        gradient.setCoordinateMode(QGradient::ObjectBoundingMode);
        auto fillColor = this->accentColor;
        fillColor.setAlpha(90);
        gradient.setColorAt(0.0, fillColor);
        fillColor.setAlpha(0);
        gradient.setColorAt(1.0, fillColor);
        this->areaSeries->setBrush(gradient);
        this->areaSeries->setPen(Qt::NoPen);

        QPen pen(this->accentColor);
        pen.setWidthF(2.5);
        this->lineSeries->setPen(pen);

        this->chart->addSeries(this->areaSeries);
        this->chart->addSeries(this->lineSeries);

        if (this->axisX == nullptr)
        {
            this->axisX = new QDateTimeAxis(this->chart);
            this->chart->addAxis(this->axisX, Qt::AlignBottom);
        }
        if (this->axisY == nullptr)
        {
            this->axisY = new QValueAxis(this->chart);
            this->chart->addAxis(this->axisY, Qt::AlignLeft);
        }

        const auto spanSeconds = qMax<qint64>(
            1, (maxTime - minTime) / 1000);
        this->axisX->setFormat(utcAxisFormat(spanSeconds));
        this->axisX->setTitleText(QStringLiteral("UTC"));
        this->axisX->setRange(QDateTime::fromMSecsSinceEpoch(minTime),
                              QDateTime::fromMSecsSinceEpoch(maxTime));

        auto minY = static_cast<double>(minBalance);
        auto maxY = static_cast<double>(
            std::max_element(this->samples.begin(), this->samples.end(),
                             [](const auto &left, const auto &right) {
                                 return left.balance < right.balance;
                             })
                ->balance);
        const auto padding = qMax(1.0, (maxY - minY) * 0.08);
        minY = qMax(0.0, minY - padding);
        maxY += padding;

        this->axisY->setRange(minY, maxY);
        this->axisY->setLabelFormat(
            maxY >= 1'000'000 ? QStringLiteral("%.1e")
                              : QStringLiteral("%.0f"));
        this->axisY->setTitleText(QStringLiteral("Channel points"));
        this->axisY->setTickCount(8);

        this->areaSeries->attachAxis(this->axisX);
        this->areaSeries->attachAxis(this->axisY);
        this->lineSeries->attachAxis(this->axisX);
        this->lineSeries->attachAxis(this->axisY);
    }

    void updateHover(const QPoint &viewportPos)
    {
        if (this->samples.isEmpty() || this->chart == nullptr ||
            this->lineSeries == nullptr)
        {
            this->clearHover();
            return;
        }

        const auto plotArea = this->chart->plotArea();
        if (!plotArea.contains(viewportPos))
        {
            this->clearHover();
            return;
        }

        const auto chartPos = this->chart->mapToValue(viewportPos,
                                                      this->lineSeries);
        const auto index = nearestSampleIndex(
            this->samples,
            QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(chartPos.x())));
        if (index < 0)
        {
            this->clearHover();
            return;
        }

        this->hoveredIndex = index;
        const auto &sample = this->samples[index];
        const auto point = this->chart->mapToPosition(
            QPointF(sample.time.toMSecsSinceEpoch(),
                    static_cast<double>(sample.balance)),
            this->lineSeries);

        if (this->crosshair == nullptr)
        {
            this->crosshair = new QGraphicsLineItem(this->chart);
            QPen crosshairPen(QColor(255, 255, 255, 80));
            crosshairPen.setStyle(Qt::DashLine);
            this->crosshair->setPen(crosshairPen);
            this->crosshair->setZValue(10);
        }
        this->crosshair->setLine(point.x(), plotArea.top(), point.x(),
                                 plotArea.bottom());
        this->crosshair->show();

        if (this->highlightDot == nullptr)
        {
            this->highlightDot = new QGraphicsEllipseItem(-5, -5, 10, 10,
                                                          this->chart);
            this->highlightDot->setBrush(this->accentColor);
            this->highlightDot->setPen(Qt::NoPen);
            this->highlightDot->setZValue(11);
        }
        this->highlightDot->setPos(point);
        this->highlightDot->show();

        QString callout = hoverTimeText(sample.time) + QStringLiteral("\n") +
                          fullBalanceText(sample.balance);
        if (index > 0)
        {
            const auto delta =
                sample.balance - this->samples[index - 1].balance;
            const auto sign = delta >= 0 ? QStringLiteral("+") : QString();
            callout += QStringLiteral("\n") + sign +
                       QLocale().toString(delta) +
                       QStringLiteral(" since previous");
        }

        this->calloutLabel->setText(callout);
        this->calloutLabel->adjustSize();

        auto calloutPos = point + QPointF(12, -this->calloutLabel->height() - 8);
        const auto maxX = plotArea.right() - this->calloutLabel->width() - 4;
        const auto maxY = plotArea.bottom() - this->calloutLabel->height() - 4;
        calloutPos.setX(qBound(plotArea.left() + 4.0, calloutPos.x(), maxX));
        calloutPos.setY(qBound(plotArea.top() + 4.0, calloutPos.y(), maxY));
        this->calloutLabel->move(calloutPos.toPoint());
        this->calloutLabel->show();
        this->calloutLabel->raise();
    }
};

ChannelPointsChartView::ChannelPointsChartView(QWidget *parent)
    : QWidget(parent)
    , d_(new Private)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    this->d_->toolbarWidget = new QWidget(this);
    this->d_->toolbarWidget->setObjectName(QStringLiteral("ChannelPointsChartToolbar"));
    auto *toolbarLayout = new QHBoxLayout(this->d_->toolbarWidget);
    toolbarLayout->setContentsMargins(8, 6, 8, 4);
    toolbarLayout->setSpacing(6);

    auto *rangeLabel = new QLabel(QStringLiteral("Range:"),
                                  this->d_->toolbarWidget);
    rangeLabel->setObjectName(QStringLiteral("ChannelPointsChartRangeLabel"));

    this->d_->rangeCombo = new QComboBox(this->d_->toolbarWidget);
    this->d_->rangeCombo->setObjectName(
        QStringLiteral("ChannelPointsChartRangeCombo"));
    populateRangeCombo(this->d_->rangeCombo);
    if (auto *listView = qobject_cast<QListView *>(this->d_->rangeCombo->view()))
    {
        listView->setSpacing(2);
        listView->setMouseTracking(true);
    }
    this->d_->rangeCombo->setToolTip(
        QStringLiteral("Show only points recorded in the selected period"));

    this->d_->resetButton = new QPushButton(QStringLiteral("Reset"),
                                            this->d_->toolbarWidget);
    this->d_->resetButton->setObjectName(
        QStringLiteral("ChannelPointsChartResetButton"));
    this->d_->resetButton->setEnabled(false);
    this->d_->resetButton->setToolTip(
        QStringLiteral("Reset the time range and chart zoom"));

    toolbarLayout->addWidget(rangeLabel);
    toolbarLayout->addWidget(this->d_->rangeCombo, 1);
    toolbarLayout->addWidget(this->d_->resetButton);

    layout->addWidget(this->d_->toolbarWidget);

    this->d_->emptyLabel = new QLabel(
        QStringLiteral("No chart data yet for this channel."), this);
    this->d_->emptyLabel->setAlignment(Qt::AlignCenter);
    this->d_->emptyLabel->setWordWrap(true);
    layout->addWidget(this->d_->emptyLabel, 1);

    this->d_->chart = new QChart();
    this->d_->chart->legend()->hide();
    this->d_->chart->setAnimationOptions(QChart::NoAnimation);
    this->d_->chart->setMargins({8, 8, 8, 8});
    this->d_->chart->setBackgroundRoundness(0);

    this->d_->chartView = new QChartView(this->d_->chart, this);
    this->d_->chartView->setRenderHint(QPainter::Antialiasing, true);
    this->d_->chartView->setRubberBand(QChartView::HorizontalRubberBand);
    this->d_->chartView->setMouseTracking(true);
    this->d_->chartView->viewport()->setMouseTracking(true);
    this->d_->chartView->viewport()->installEventFilter(this);
    layout->addWidget(this->d_->chartView, 1);

    this->d_->calloutLabel = new QLabel(this->d_->chartView);
    this->d_->calloutLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    this->d_->calloutLabel->setObjectName("ChannelPointsChartCallout");
    this->d_->calloutLabel->hide();

    QObject::connect(this->d_->rangeCombo,
                     QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                     [this](int index) {
                         this->d_->selectedRange = chartTimeRangeFromCombo(
                             this->d_->rangeCombo, index);
                         this->d_->applyTimeRangeFilter();
                     });
    QObject::connect(this->d_->resetButton, &QPushButton::clicked, this,
                     [this] {
                         this->resetTimeRange();
                     });

    this->d_->toolbarWidget->setVisible(false);
}

ChannelPointsChartView::~ChannelPointsChartView()
{
    delete this->d_;
}

void ChannelPointsChartView::setSamples(
    const QVector<ChannelPointsChartSample> &samples)
{
    this->d_->allSamples = samples;
    this->d_->applyTimeRangeFilter();
}

void ChannelPointsChartView::reloadFromStore(const QString &channelId)
{
    this->setSamples(ChannelPointsChartStore::samplesForChannel(channelId));
}

void ChannelPointsChartView::resetTimeRange()
{
    if (this->d_->rangeCombo != nullptr)
    {
        QSignalBlocker blocker(this->d_->rangeCombo);
        setComboToChartTimeRange(this->d_->rangeCombo,
                                 DEFAULT_CHART_TIME_RANGE);
    }

    this->d_->selectedRange = DEFAULT_CHART_TIME_RANGE;
    if (this->d_->chart != nullptr)
    {
        this->d_->chart->zoomReset();
    }
    this->d_->applyTimeRangeFilter();
}

void ChannelPointsChartView::applyTheme(const Theme &theme)
{
    const auto background = theme.window.background;
    const auto text = theme.window.text;
    auto muted = text;
    muted.setAlpha(180);
    auto grid = theme.splits.header.border;
    grid.setAlpha(90);
    auto buttonHoverBg = theme.tabs.regular.backgrounds.hover;
    auto fieldBg = theme.splits.input.background;
    auto popupBg = background.darker(theme.isLightTheme() ? 102 : 108);
    auto itemHoverBg = ACCENT_COLOR;
    itemHoverBg.setAlpha(theme.isLightTheme() ? 48 : 72);
    auto itemSelectedBg = ACCENT_COLOR;
    itemSelectedBg.setAlpha(theme.isLightTheme() ? 88 : 110);

    this->d_->chart->setBackgroundBrush(background);
    this->d_->chart->setPlotAreaBackgroundBrush(background);
    this->d_->chart->setTitleBrush(text);

    if (this->d_->axisX != nullptr)
    {
        this->d_->axisX->setLabelsColor(muted);
        this->d_->axisX->setTitleBrush(muted);
        this->d_->axisX->setGridLineColor(grid);
        this->d_->axisX->setMinorGridLineColor(grid);
    }
    if (this->d_->axisY != nullptr)
    {
        this->d_->axisY->setLabelsColor(muted);
        this->d_->axisY->setTitleBrush(muted);
        this->d_->axisY->setGridLineColor(grid);
        this->d_->axisY->setMinorGridLineColor(grid);
    }

    this->d_->emptyLabel->setStyleSheet(
        QStringLiteral("color: %1;").arg(muted.name(QColor::HexArgb)));

    this->d_->calloutLabel->setStyleSheet(QStringLiteral(
        "QLabel#ChannelPointsChartCallout {"
        " background-color: %1;"
        " color: %2;"
        " border: 1px solid %3;"
        " border-radius: 6px;"
        " padding: 6px 8px;"
        "}")
                                              .arg(background.darker(115)
                                                       .name(QColor::HexArgb),
                                                   text.name(QColor::HexArgb),
                                                   grid.name(QColor::HexArgb)));

    this->d_->chartView->setStyleSheet(
        QStringLiteral("background: %1; border: none;")
            .arg(background.name(QColor::HexArgb)));

    if (this->d_->toolbarWidget != nullptr)
    {
        this->d_->toolbarWidget->setStyleSheet(QStringLiteral(R"(
            QWidget#ChannelPointsChartToolbar {
                background: transparent;
            }
            QLabel#ChannelPointsChartRangeLabel {
                color: %2;
            }
            QComboBox#ChannelPointsChartRangeCombo,
            QPushButton#ChannelPointsChartResetButton {
                background: %4;
                color: %1;
                border: 1px solid %3;
                border-radius: 4px;
                padding: 3px 8px;
                min-height: 22px;
            }
            QComboBox#ChannelPointsChartRangeCombo:hover,
            QPushButton#ChannelPointsChartResetButton:hover:enabled {
                background: %5;
            }
            QPushButton#ChannelPointsChartResetButton:disabled {
                color: %2;
            }
            QComboBox#ChannelPointsChartRangeCombo::drop-down {
                border: 0;
                width: 18px;
            }
            QComboBox#ChannelPointsChartRangeCombo QAbstractItemView {
                background: %6;
                color: %1;
                border: 1px solid %3;
                border-radius: 6px;
                outline: none;
                padding: 4px;
                selection-background-color: transparent;
                selection-color: %1;
            }
            QComboBox#ChannelPointsChartRangeCombo QAbstractItemView::item {
                padding: 6px 10px;
                border-radius: 4px;
                min-height: 22px;
            }
            QComboBox#ChannelPointsChartRangeCombo QAbstractItemView::item:hover {
                background: %7;
                color: %1;
            }
            QComboBox#ChannelPointsChartRangeCombo QAbstractItemView::item:selected {
                background: %8;
                color: %1;
                font-weight: 600;
            }
        )")
                                                   .arg(text.name(QColor::HexArgb),
                                                        muted.name(QColor::HexArgb),
                                                        grid.name(QColor::HexArgb),
                                                        fieldBg.name(QColor::HexArgb),
                                                        buttonHoverBg.name(
                                                            QColor::HexArgb),
                                                        popupBg.name(QColor::HexArgb),
                                                        itemHoverBg.name(
                                                            QColor::HexArgb),
                                                        itemSelectedBg.name(
                                                            QColor::HexArgb)));
    }
}

bool ChannelPointsChartView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == this->d_->chartView->viewport())
    {
        switch (event->type())
        {
            case QEvent::MouseMove: {
                auto *mouseEvent = static_cast<QMouseEvent *>(event);
                this->d_->updateHover(mouseEvent->pos());
                return false;
            }
            case QEvent::Leave: {
                this->d_->clearHover();
                return false;
            }
            case QEvent::MouseButtonRelease: {
                QTimer::singleShot(0, this, [this] {
                    this->d_->updateResetButton();
                });
                return false;
            }
            default:
                break;
        }
    }

    return QWidget::eventFilter(watched, event);
}

}  // namespace chatterino

#endif

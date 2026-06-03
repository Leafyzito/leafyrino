#pragma once

#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "widgets/DraggablePopup.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QDateTime>
#include <QHash>
#include <QLabel>
#include <QPointer>
#include <QStringList>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QVector>

#include <optional>
#include <vector>

class QScrollArea;
class QComboBox;
class QShowEvent;
class QPushButton;
class QResizeEvent;

namespace chatterino {

class Button;
class PredictionTemplatePicker;
class SvgButton;

class PredictionDialog : public DraggablePopup
{
public:
    PredictionDialog(TwitchChannel *channel, QWidget *parent = nullptr);

    static void showDialog(TwitchChannel *channel, QWidget *parent,
                           const std::optional<TwitchChannel::PredictionEvent>
                               &prediction = std::nullopt);

    void setPrediction(
        const std::optional<TwitchChannel::PredictionEvent> &prediction);

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildCreateUI();
    void buildManageUI();
    void buildBettingUI();
    void updateUI();
    void updateInPlace();
    void updateManageOutcomeSelection();
    void settleLayoutAfterResize();
    void fitBettingOutcomesList();
    void applySizeConstraints(bool preserveCurrentPosition);
    void refreshHeader();
    void refreshStyle();
    void ensureCreateDraft();
    void startPrediction();
    bool isBroadcasterView() const;
    bool populatePredictionTemplates(PredictionTemplatePicker *picker);
    void fetchPredictionTemplates(PredictionTemplatePicker *picker);

    TwitchChannel *channel_;
    std::optional<TwitchChannel::PredictionEvent> currentPrediction_;

    QVBoxLayout *mainLayout_{};
    QWidget *headerWidget_{};
    QLabel *headerTitleLabel_{};
    QLabel *headerSubtitleLabel_{};
    Button *pinButton_{};
    QPushButton *modToggleButton_{};
    SvgButton *closeButton_{};
    QScrollArea *scrollArea_{};
    QScrollArea *outcomesScrollArea_ = nullptr;
    QWidget *bettingOutcomesPanel_ = nullptr;
    QComboBox *manageResolveCombo_ = nullptr;
    QWidget *activeWidget_ = nullptr;
    QWidget *bottomWidget_ = nullptr;

    QString draftTitle_;
    QStringList draftOutcomes_;
    QString selectedBettingOutcomeId_;
    QString selectedManageOutcomeId_;
    int draftDurationSeconds_ = 120;
    int bettingWagerAmount_ = 0;
    bool createInFlight_ = false;
    bool modBettingView_ = false;
    int mainScrollValue_ = 0;
    int outcomesScrollValue_ = 0;
    int updateGuard_ = 0;
    int layoutUpdateGeneration_ = 0;
    QVector<PredictionTemplate> predictionTemplates_;
    QDateTime predictionTemplatesFetchedAt_;
    QString predictionTemplatesError_;
    bool predictionTemplatesFetchInFlight_ = false;
    bool renderedBroadcasterView_ = false;

    std::vector<pajlada::Signals::ScopedConnection> managedConnections_;

    QVariantAnimation *barsAnim_{};
    QHash<QString, int> previousBarWidths_;
    QHash<QString, int> targetBarWidths_;

    static std::vector<QPointer<PredictionDialog>> activeDialogs_;
};

}  // namespace chatterino

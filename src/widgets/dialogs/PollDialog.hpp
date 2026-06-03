#pragma once

#include "providers/twitch/TwitchChannel.hpp"
#include "widgets/DraggablePopup.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QDateTime>
#include <QHash>
#include <QPointer>

#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QShowEvent;
class QTimer;
class QVBoxLayout;

namespace chatterino {

class Button;
class SvgButton;

class PollDialog : public DraggablePopup
{
public:
    enum class OpenMode {
        CreateOrActivePoll,
        ShowPollResults,
    };

    PollDialog(TwitchChannel *channel, QWidget *parent = nullptr);

    static void showDialog(
        TwitchChannel *channel, QWidget *parent,
        const std::optional<TwitchChannel::PollEvent> &poll = std::nullopt,
        OpenMode mode = OpenMode::CreateOrActivePoll);

    void setPoll(const std::optional<TwitchChannel::PollEvent> &poll);

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateUi();
    void refreshStyle();
    void refreshHeader();
    void buildCreateUi();
    void buildVoteUi();
    void ensureDraft();
    void createPoll();
    void castVote(bool extraVote);
    int currentUserTotalVotes() const;
    void applySizeConstraints(bool preserveCurrentPosition = true);
    void fitCreateOptionsList();
    void fitVoteOptionsList();
    int remainingPollSeconds() const;
    void updateCountdownLabel();
    void updateCountdownTimer();
    bool isBroadcasterView() const;

    TwitchChannel *channel_{};
    std::optional<TwitchChannel::PollEvent> currentPoll_;
    QDateTime pollSnapshotAt_;

    QVBoxLayout *mainLayout_{};
    QWidget *headerWidget_{};
    QLabel *headerTitleLabel_{};
    QLabel *headerSubtitleLabel_{};
    Button *pinButton_{};
    SvgButton *closeButton_{};
    QScrollArea *scrollArea_{};
    QWidget *activeWidget_{};
    QWidget *bottomWidget_{};
    QScrollArea *outcomesScrollArea_{};
    QWidget *createOutcomesPanel_{};
    QWidget *voteQuestionCard_{};
    QWidget *voteOutcomesPanel_{};
    QLabel *countdownLabel_{};
    QTimer *countdownTimer_{};

    QString draftTitle_;
    QStringList draftChoices_;
    int draftDurationSeconds_ = 120;
    bool draftEnableAdditionalVotes_ = false;
    int draftPointsPerVote_ = 10;
    QString selectedChoiceId_;
    int outcomesScrollValue_ = 0;
    QHash<QString, double> pollChoiceFillProgress_;
    bool suppressSelectedChoiceOutline_ = false;
    bool actionInFlight_ = false;
    bool pollExpiryRefreshQueued_ = false;
    bool showInactivePollResults_ = false;
    int updateGuard_ = 0;
    int mainScrollValue_ = 0;
    std::vector<pajlada::Signals::ScopedConnection> managedConnections_;

    static std::vector<QPointer<PollDialog>> activeDialogs_;
};

}  // namespace chatterino

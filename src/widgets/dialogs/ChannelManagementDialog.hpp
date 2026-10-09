#pragma once

#include "providers/twitch/ChannelManagement.hpp"
#include "widgets/DraggablePopup.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QHash>
#include <QStringList>
#include <QTimer>

#include <memory>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
class QToolButton;
class QWidget;

namespace chatterino {

class Button;
class ChannelCategoryPicker;
class FlowLayout;
class SvgButton;
class TwitchChannel;

class ChannelManagementDialog : public DraggablePopup
{
public:
    static void showForChannel(const std::shared_ptr<TwitchChannel> &channel,
                               QWidget *parent = nullptr);

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    void windowDeactivationEvent() override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    explicit ChannelManagementDialog(std::shared_ptr<TwitchChannel> channel,
                                     QWidget *parent = nullptr);

    void loadState();
    void prepareLoadingState();
    void setLoadingVisual(bool loading);
    void applyState(ChannelManagementState state);
    void refreshParentHeaderIcons() const;
    void saveMetadata();

    void addTagFromInput();
    void removeTag(const QString &tag);
    void rebuildTagChips();
    void rebuildContentLabels();

    void handleCommercialClick();
    void startCommercial();
    void resetCommercialConfirmation();
    void setCommercialMessage(const QString &text, bool error = false);
    void updateCommercialAvailability();

    void updateTitleCounter();
    void updateControls();
    void refreshStyle();
    void fitToContent();
    void setStatus(const QString &text, bool error = false);
    void setCategoryHint(const QString &text, bool error = false);

    [[nodiscard]] ChannelManagementUpdate buildUpdate() const;
    [[nodiscard]] std::vector<ChannelManagementContentLabel>
        currentContentLabels() const;
    [[nodiscard]] bool hasInvalidCategoryText() const;
    [[nodiscard]] bool hasInvalidTitle() const;

    std::shared_ptr<TwitchChannel> channel_;

    QWidget *headerWidget_{};
    QLabel *headerTitleLabel_{};
    QLabel *headerSubtitleLabel_{};
    Button *pinButton_{};
    SvgButton *closeButton_{};
    QProgressBar *loadingBar_{};

    QScrollArea *scrollArea_{};
    QWidget *contentWidget_{};
    QVBoxLayout *contentLayout_{};

    QPlainTextEdit *titleEdit_{};
    QLabel *titleCounterLabel_{};
    QLabel *titleValidationLabel_{};
    ChannelCategoryPicker *categoryPicker_{};
    QLabel *categoryStatusLabel_{};
    QLabel *categoryHintLabel_{};

    QWidget *tagChipsWidget_{};
    QWidget *tagEditor_{};
    FlowLayout *tagFlow_{};
    QLineEdit *tagInput_{};
    QPushButton *addTagButton_{};
    QLabel *tagCountLabel_{};
    QLabel *tagHintLabel_{};
    QComboBox *languageCombo_{};

    QToolButton *contentLabelsToggle_{};
    QLabel *contentLabelsSummary_{};
    QWidget *contentLabelsWidget_{};
    QVBoxLayout *contentLabelsLayout_{};
    QHash<QString, QCheckBox *> contentLabelChecks_;
    QCheckBox *rerunCheck_{};

    QLabel *commercialAvailabilityLabel_{};
    QComboBox *commercialLengthCombo_{};
    QPushButton *runCommercialButton_{};
    QLabel *commercialConfirmationLabel_{};

    QWidget *footerWidget_{};
    QLabel *statusLabel_{};
    QPushButton *cancelButton_{};
    QPushButton *saveButton_{};

    QTimer commercialConfirmationTimer_;
    QTimer commercialCooldownTimer_;
    pajlada::Signals::ScopedConnection streamStatusConnection_;

    ChannelManagementMetadata originalMetadata_;
    QStringList stagedTags_;

    bool loaded_ = false;
    bool loadInFlight_ = false;
    bool saveInFlight_ = false;
    bool commercialInFlight_ = false;
    bool commercialConfirmationArmed_ = false;
    bool resizeQueued_ = false;
    QString statusText_;
    bool statusIsError_ = false;
    QString categoryHintText_;
    bool categoryHintIsError_ = false;
    QString commercialMessage_;
    bool commercialMessageIsError_ = false;
};

}  // namespace chatterino

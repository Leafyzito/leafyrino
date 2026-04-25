#include "widgets/settingspages/LeafyrinoPage.hpp"

#include "singletons/Settings.hpp"
#include "widgets/BaseWindow.hpp"
#include "widgets/settingspages/GeneralPageView.hpp"
#include "widgets/settingspages/SettingWidget.hpp"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QUrl>

namespace chatterino {

LeafyrinoPage::LeafyrinoPage()
{
    auto *outer = new QVBoxLayout;
    auto *inner = new QHBoxLayout;
    auto *view = GeneralPageView::withNavigation(this);
    this->view_ = view;

    inner->addWidget(view);
    auto *frame = new QFrame;
    frame->setLayout(inner);
    outer->addWidget(frame);
    this->setLayout(outer);

    this->initLayout(*view);
}

bool LeafyrinoPage::filterElements(const QString &query)
{
    if (this->view_)
    {
        return this->view_->filterElements(query) || query.isEmpty();
    }

    return false;
}

void LeafyrinoPage::initLayout(GeneralPageView &layout)
{
    auto &s = *getSettings();

    layout.addTitle("Badges");
    SettingWidget::checkbox("Homies", s.showBadgesHomies)
        ->addKeywords({"homies"})
        ->setTooltip("Homies supporter badges and custom badges")
        ->addTo(layout);
    SettingWidget::checkbox("Folhinha", s.showBadgesFolhinha)
        ->addKeywords({"folhinha", "folhinhabot"})
        ->setTooltip("FolhinhaBot Plus and Founder badges")
        ->addTo(layout);

    layout.addTitle("Userinfo popup");
    SettingWidget::checkbox("Show chatters", s.showUserinfoPopupChatters)
        ->addKeywords(
            {"userinfo", "user card", "usercard", "popup", "chatters"})
        ->addTo(layout);
    SettingWidget::checkbox("Show last live date", s.showUserinfoPopupLastLive)
        ->addKeywords(
            {"userinfo", "user card", "usercard", "popup", "last live"})
        ->addTo(layout);
    SettingWidget::checkbox("Show color", s.showUserinfoPopupColor)
        ->addKeywords({"userinfo", "user card", "usercard", "popup", "color"})
        ->addTo(layout);

    layout.addTitle("Panels");
    SettingWidget::checkbox("Show pinned message panel in splits",
                            s.showPinnedMessagePanel)
        ->setTooltip("Polls Twitch for the current moderator-pinned chat "
                     "message and shows it above chat.")
        ->addTo(layout);
    SettingWidget::checkbox("Show active prediction panel in splits",
                            s.showPredictionPanel)
        ->setTooltip("Polls Twitch for an active or locked channel points "
                     "prediction and shows it above chat.")
        ->addTo(layout);

    SettingWidget::checkbox("Play sound when a new prediction starts",
                            s.predictionStartPlaySound)
        ->addKeywords({"prediction", "sound", "ping"})
        ->addTo(layout);
    SettingWidget::checkbox("Custom sound for prediction start",
                            s.predictionStartCustomSound)
        ->addKeywords({"prediction", "sound", "custom"})
        ->conditionallyEnabledBy(s.predictionStartPlaySound)
        ->addTo(layout);

    // Custom sound file picker row
    {
        auto *row = new QWidget(this);
        auto *hl = new QHBoxLayout(row);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(6);

        auto *label = new QLabel(row);
        label->setTextFormat(Qt::RichText);
        label->setTextInteractionFlags(Qt::TextBrowserInteraction |
                                       Qt::LinksAccessibleByKeyboard);
        label->setOpenExternalLinks(true);

        auto *clearBtn = new QPushButton(QStringLiteral("Clear"), row);
        auto *changeBtn = new QPushButton(QStringLiteral("Change..."), row);

        hl->addWidget(label, 1);
        hl->addWidget(changeBtn, 0);
        hl->addWidget(clearBtn, 0);

        const auto updateUi = [label, changeBtn, clearBtn, &s] {
            const bool enabled =
                s.predictionStartPlaySound && s.predictionStartCustomSound;
            label->setEnabled(enabled);
            changeBtn->setEnabled(enabled);
            clearBtn->setEnabled(enabled);

            const QString value = s.predictionStartSoundPath.getValue();
            if (value.trimmed().isEmpty())
            {
                label->setText(QStringLiteral(
                    "Prediction start sound: Default (Chatterino Ping)"));
                clearBtn->hide();
                return;
            }

            const QUrl url = QUrl::fromLocalFile(value);
            label->setText(
                QStringLiteral("Prediction start sound: <a href=\"%1\">%2</a>")
                    .arg(url.toString(QUrl::FullyEncoded), url.fileName()));
            clearBtn->show();
        };

        QObject::connect(changeBtn, &QPushButton::clicked, this, [&s] {
            const auto fileName = QFileDialog::getOpenFileName(
                nullptr, QObject::tr("Open Sound"), "",
                QObject::tr("Audio Files (*.mp3 *.wav)"));
            s.predictionStartSoundPath = fileName;
        });
        QObject::connect(clearBtn, &QPushButton::clicked, this, [&s] {
            s.predictionStartSoundPath = QString();
        });

        s.predictionStartPlaySound.connect(
            [updateUi](const bool &, const auto &) {
                updateUi();
            });
        s.predictionStartCustomSound.connect(
            [updateUi](const bool &, const auto &) {
                updateUi();
            });
        s.predictionStartSoundPath.connect(
            [updateUi](const QString &, const auto &) {
                updateUi();
            });
        updateUi();

        layout.addWidget(row);
    }

    layout.addTitle("Messages per second");
    SettingWidget::checkbox("Show messages-per-second (mps) overlay in splits",
                            s.showSplitMps)
        ->setTooltip("Shows a faint overlay label (e.g. \"12 mps\") of how "
                     "many messages are being sent per second.")
        ->addTo(layout);

    SettingWidget::dropdown("MPS overlay position", s.splitMpsCorner)
        ->conditionallyEnabledBy(s.showSplitMps)
        ->addTo(layout);

    SettingWidget::checkbox("Show 0 mps", s.showSplitMpsWhenZero)
        ->conditionallyEnabledBy(s.showSplitMps)
        ->addTo(layout);

    layout.addTitle("Tabs");
    SettingWidget::checkbox("Always use theme color for tab highlights",
                            s.tabHighlightsUseThemeColor)
        ->setTooltip("Use the theme's default highlighted tab color instead of "
                     "per-highlight colors.")
        ->addTo(layout);

    layout.addStretch();

    // Invisible element for width
    auto *inv = new BaseWidget(this);
    layout.addWidget(inv);
}

}  // namespace chatterino

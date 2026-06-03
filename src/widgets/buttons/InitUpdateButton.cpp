// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/buttons/InitUpdateButton.hpp"

#include "Application.hpp"
#include "widgets/buttons/PixmapButton.hpp"
#include "widgets/dialogs/UpdateDialog.hpp"

namespace chatterino {

void initUpdateButton(PixmapButton &button,
                      const std::function<void()> &relayout,
                      pajlada::Signals::SignalHolder &signalHolder)
{
    button.hide();

    QObject::connect(&button, &Button::leftClicked, [&button, relayout] {
        auto *dialog = new UpdateDialog();

        auto globalPoint = button.mapToGlobal(
            QPoint(int(-100 * button.scale()), button.height()));

        if (globalPoint.x() < 0)
        {
            globalPoint.setX(0);
        }

        dialog->moveTo(globalPoint, widgets::BoundsChecking::DesiredPosition);
        dialog->show();
        dialog->raise();

        std::ignore =
            dialog->buttonClicked.connect([&button, relayout](auto buttonType) {
                switch (buttonType)
                {
                    case UpdateDialog::Dismiss: {
                        button.hide();
                        relayout();
                    }
                    break;
                    case UpdateDialog::Install: {
                        getApp()->getUpdates().installUpdates();
                    }
                    break;
                }
            });

    });

    auto updateChange = [&button, relayout](auto) {
        button.setVisible(getApp()->getUpdates().shouldShowUpdateButton());

        const auto *imageUrl = getApp()->getUpdates().isError()
                                   ? ":/buttons/updateError.png"
                                   : ":/buttons/update.png";
        button.setPixmap(QPixmap(imageUrl));

        relayout();
    };

    updateChange(getApp()->getUpdates().getStatus());

    signalHolder.managedConnect(getApp()->getUpdates().statusUpdated,
                                [updateChange](auto status) {
                                    updateChange(status);
                                });
}

}

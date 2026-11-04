// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/HiddenSeventvPaintsDialog.hpp"

#include "providers/seventv/SeventvHiddenPaintModel.hpp"
#include "singletons/Settings.hpp"
#include "widgets/helper/EditableModelView.hpp"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QTableView>
#include <QVBoxLayout>

namespace chatterino {

HiddenSeventvPaintsDialog::HiddenSeventvPaintsDialog(QWidget *parent)
    : QDialog(parent)
{
    this->setWindowTitle(QStringLiteral("Hidden 7TV paints"));
    this->setModal(true);
    this->setMinimumSize(420, 320);

    auto *layout = new QVBoxLayout(this);

    auto *description = new QLabel(
        QStringLiteral(
            "These paints won't be rendered in chat. To hide a paint, open "
            "the usercard of someone wearing it and click \"Hide\" next to "
            "the paint (requires \"Show 7TV paint\" under Leafyrino → "
            "Usercard)."),
        this);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *view = new EditableModelView(
        (new SeventvHiddenPaintModel(nullptr))
            ->initialized(&getSettings()->hiddenSevenTVPaints),
        false);
    view->setAddButtonVisible(false);
    view->setTitles({"Name", "ID"});
    view->getTableView()->horizontalHeader()->setSectionResizeMode(
        QHeaderView::Stretch);
    layout->addWidget(view, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                     &QDialog::reject);
    layout->addWidget(buttons);
}

}  // namespace chatterino

// SPDX-FileCopyrightText: 2026 Contributors to leafyrino
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/MoltorinoAuthDialog.hpp"

#include "widgets/dialogs/KickLoginPage.hpp"
#include "widgets/dialogs/MoltorinoAuthPage.hpp"

#include <QDialog>
#include <QDialogButtonBox>
#include <QTabWidget>
#include <QVBoxLayout>

namespace chatterino {

void showMoltorinoAuthDialog(QWidget *parent, const QString &windowTitle,
                             bool includeKickTab)
{
    QDialog dialog(parent);
    dialog.setMinimumWidth(430);
    dialog.setWindowFlags(
        (dialog.windowFlags() & ~(Qt::WindowContextHelpButtonHint)) |
        Qt::Dialog | Qt::MSWindowsFixedSizeDialogHint);
    dialog.setWindowTitle(windowTitle.isEmpty()
                              ? QStringLiteral("Manage Accounts")
                              : windowTitle);

    auto *layout = new QVBoxLayout(&dialog);
    auto *page = createMoltorinoAuthLoginPage(&dialog);
    layout->addWidget(page);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttonBox, &QDialogButtonBox::rejected, &dialog,
                     &QDialog::close);
    layout->addWidget(buttonBox);

    if (includeKickTab)
    {
        if (auto *tabs = moltorinoAuthLoginPageTabs(page))
        {
            tabs->addTab(new KickLoginPage(), QStringLiteral("Kick"));
        }
    }

    dialog.exec();
}

}  // namespace chatterino

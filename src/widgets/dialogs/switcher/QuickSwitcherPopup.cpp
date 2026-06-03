// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/switcher/QuickSwitcherPopup.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/dialogs/switcher/NewPopupItem.hpp"
#include "widgets/dialogs/switcher/NewTabItem.hpp"
#include "widgets/dialogs/switcher/SwitchSplitItem.hpp"
#include "widgets/helper/NotebookTab.hpp"
#include "widgets/listview/GenericListView.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

namespace {

using namespace chatterino;

QList<SplitContainer *> openPages(Window *window)
{
    QList<SplitContainer *> pages;

    auto &nb = window->getNotebook();
    for (int i = 0; i < nb.getPageCount(); ++i)
    {
        pages.append(static_cast<SplitContainer *>(nb.getPageAt(i)));
    }

    return pages;
}

}

namespace chatterino {

QuickSwitcherPopup::QuickSwitcherPopup(Window *parent)
    : BasePopup(
          {
              BaseWindow::Flags::Frameless,
              BaseWindow::Flags::TopMost,
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
          },
          parent)
    , switcherModel_(this)
    , window(parent)
{
    assert(this->windowFlags().testFlag(Qt::Dialog));

    this->windowDeactivateAction = WindowDeactivateAction::Delete;
    this->setMinimumSize(QuickSwitcherPopup::MINIMUM_SIZE);

    this->initWidgets();

    const QRect geom = parent->geometry();

    this->setGeometry(QStyle::alignedRect(Qt::LeftToRight, Qt::AlignCenter,
                                          this->size(), geom));

    this->themeChangedEvent();

    this->installEventFilter(this->ui_.list);
}

void QuickSwitcherPopup::initWidgets()
{
    LayoutCreator<QWidget> creator(this->BaseWindow::getLayoutContainer());
    auto vbox = creator.setLayoutType<QVBoxLayout>();

    {
        auto lineEdit = vbox.emplace<QLineEdit>().assign(&this->ui_.searchEdit);
        lineEdit->setPlaceholderText("Jump to a channel or open a new one");
        QObject::connect(this->ui_.searchEdit, &QLineEdit::textChanged, this,
                         &QuickSwitcherPopup::updateSuggestions);
    }

    {
        auto listView = vbox.emplace<GenericListView>().assign(&this->ui_.list);
        listView->setModel(&this->switcherModel_);

        QObject::connect(listView.getElement(),
                         &GenericListView::closeRequested, this, [this] {
                             this->close();
                         });

        this->ui_.searchEdit->installEventFilter(listView.getElement());
    }
}

void QuickSwitcherPopup::updateSuggestions(const QString &text)
{
    this->switcherModel_.clear();

    for (auto *sc : openPages(this->window))
    {
        const QString &tabTitle = sc->getTab()->getTitle();
        const auto splits = sc->getSplits();

        for (auto *split : splits)
        {
            if (split->getChannel()->getName().contains(text,
                                                        Qt::CaseInsensitive))
            {
                auto item = std::make_unique<SwitchSplitItem>(sc, split);
                this->switcherModel_.addItem(std::move(item));

                goto nextPage;
            }
        }

        if (tabTitle.contains(text, Qt::CaseInsensitive))
        {
            auto item = std::make_unique<SwitchSplitItem>(sc);
            this->switcherModel_.addItem(std::move(item));
            continue;
        }

    nextPage:;
    }

    if (!text.isEmpty())
    {
        auto newTabItem = std::make_unique<NewTabItem>(this->window, text);
        this->switcherModel_.addItem(std::move(newTabItem));

        auto newPopupItem = std::make_unique<NewPopupItem>(text);
        this->switcherModel_.addItem(std::move(newPopupItem));
    }

    const auto &startIdx = this->switcherModel_.index(0);
    this->ui_.list->setCurrentIndex(startIdx);

    QTimer::singleShot(0, this, [this] {
        this->adjustSize();
    });
}

void QuickSwitcherPopup::themeChangedEvent()
{
    BasePopup::themeChangedEvent();

    this->ui_.searchEdit->setStyleSheet(this->theme->splits.input.styleSheet);
    this->ui_.list->refreshTheme(*this->theme);
}

}

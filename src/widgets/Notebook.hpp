// SPDX-FileCopyrightText: 2016 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/TabHistory.hpp"
#include "widgets/BaseWidget.hpp"
#include "widgets/NotebookEnums.hpp"

#include <boost/signals2.hpp>
#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QList>
#include <QMenu>
#include <QMessageBox>
#include <QWidget>

#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace chatterino {

class Button;
class PixmapButton;
class Window;
class DrawnButton;
class NotebookTab;
class SplitContainer;
class Split;

using TabVisibilityFilter = std::function<bool(const NotebookTab *)>;

class Notebook : public BaseWidget
{
    Q_OBJECT

public:
    explicit Notebook(QWidget *parent);
    ~Notebook() override = default;

    NotebookTab *addPage(QWidget *page, QString title = QString(),
                         bool select = false);

    NotebookTab *addPageAt(QWidget *page, int position,
                           QString title = QString(), bool select = false);
    void removePage(QWidget *page);
    void duplicatePage(QWidget *page);
    void removeCurrentPage();

    int indexOf(QWidget *page) const;

    int visibleIndexOf(QWidget *page) const;

    int getVisibleTabCount() const;

    /**
     * @brief Selects the Notebook tab containing the given page.
     **/
    virtual void select(QWidget *page, bool focusPage = true,
                        bool recordInHistory = true);

    void selectHistoryBack(bool focusPage);
    void selectHistoryForward(bool focusPage);
    QWidget *getPreviousVisitedPage() const;
    std::vector<QWidget *> getVisitHistoryPages() const;

    void selectIndex(int index, bool focusPage = true);

    void selectVisibleIndex(int index, bool focusPage = true);

    /**
     * @brief Selects the next visible tab. Wraps to the start if required. 
     **/
    void selectNextTab(bool focusPage = true, bool recordInHistory = true);

    /**
     * @brief Selects the previous visible tab. Wraps to the end if required. 
     **/
    void selectPreviousTab(bool focusPage = true, bool recordInHistory = true);

    void selectLastTab(bool focusPage = true);

    int getPageCount() const;
    QWidget *getPageAt(int index) const;
    int getSelectedIndex() const;
    QWidget *getSelectedPage() const;

    QWidget *tabAt(QPoint point, int &index, int maxWidth = 2000000000);
    void rearrangePage(QWidget *page, int index);

    bool getAllowUserTabManagement() const;
    void setAllowUserTabManagement(bool value);

    bool getShowAddButton() const;
    void setShowAddButton(bool value);

    void setTabLocation(NotebookTabLocation location);

    bool isNotebookLayoutLocked() const;
    virtual void setLockNotebookLayout(bool value);

    virtual void addNotebookActionsToMenu(QMenu *menu);

    void refresh();

protected:
    bool getShowTabs() const;
    void setShowTabs(bool value);

    void scaleChangedEvent(float scale_) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *) override;

    DrawnButton *addButton_;

    template <typename T>
    T *addCustomButton(auto &&...args)
    {
        auto *btn = new T(std::forward<decltype(args)>(args)..., this);
        this->customButtons_.push_back(btn);

        return btn;
    }

    struct Item {
        NotebookTab *tab{};
        QWidget *page{};
        QWidget *selectedWidget{};
    };

    const QList<Item> items()
    {
        return this->items_;
    }

    void setTabVisibilityFilter(TabVisibilityFilter filter);

    bool shouldShowTab(const NotebookTab *tab) const;

    void performLayout(bool animate = false);

    void sortTabsAlphabetically();

private:
    struct LayoutContext {
        int left = 0;
        int right = 0;
        int bottom = 0;
        float scale = 0;
        int tabHeight = 0;
        int minimumTabAreaSpace = 0;
        int addButtonWidth = 0;
        int lineThickness = 0;
        int tabSpacer = 0;

        int buttonWidth = 0;
        int buttonHeight = 0;

        std::span<Item> items;
    };

    void performHorizontalLayout(const LayoutContext &ctx, bool animated);
    void performVerticalLayout(const LayoutContext &ctx, bool animated);

    void showTabVisibilityInfoPopup();

    void updateTabVisibility();
    void resizeAddButton();

    bool containsPage(QWidget *page) const;
    std::optional<Item> findItem(QWidget *page);

    void pruneInvalidHistoryEntries();

    static bool containsChild(const QObject *obj, const QObject *child);
    NotebookTab *getTabFromPage(QWidget *page);

    size_t visibleButtonCount() const;

    QList<Item> items_;
    QMenu *menu_ = nullptr;
    QWidget *selectedPage_ = nullptr;

    TabHistory tabHistory_;

    std::vector<Button *> customButtons_;

    bool allowUserTabManagement_ = false;
    bool showTabs_ = true;
    bool showAddButton_ = false;
    int lineOffset_ = 20;
    bool lockNotebookLayout_ = false;

    bool refreshPaused_ = false;
    bool refreshRequested_ = false;

    NotebookTabLocation tabLocation_ = NotebookTabLocation::Top;

    QAction *lockNotebookLayoutAction_;
    QAction *toggleTopMostAction_;

    TabVisibilityFilter tabVisibilityFilter_;
};

class SplitNotebook : public Notebook
{
public:
    SplitNotebook(Window *parent);

    SplitContainer *addPage(bool select = false);
    SplitContainer *getOrAddSelectedPage();

    SplitContainer *getSelectedPage();
    void select(QWidget *page, bool focusPage = true,
                bool recordInHistory = true) override;
    void themeChangedEvent() override;

    void addNotebookActionsToMenu(QMenu *menu) override;

    void forEachSplit(const std::function<void(Split *)> &cb);

    void toggleTabVisibility();

    QAction *showAllTabsAction;
    QAction *onlyShowLiveTabsAction;
    QAction *hideAllTabsAction;

protected:
    void showEvent(QShowEvent *event) override;

private:
    QAction *sortTabsAlphabeticallyAction_;

    void addCustomButtons();

    pajlada::Signals::SignalHolder signalHolder_;
    boost::signals2::scoped_connection currentUserChangedConnection_;

    PixmapButton *streamerModeIcon_{};
    void updateStreamerModeIcon();

    void setLockNotebookLayout(bool value) override;
};

}  // namespace chatterino

#pragma once

#include <QHBoxLayout>
#include <QStackedLayout>
#include <QString>
#include <QWidget>

#include <vector>

namespace chatterino {

class MicroNotebook : public QWidget
{
public:
    MicroNotebook(QWidget *parent = nullptr);

    int addPage(QWidget *page, QString name);

    void select(QWidget *page);

    bool isSelected(QWidget *page) const;

    void setShowHeader(bool showHeader);

private:
    struct Item {
        QString name;
        int index;
    };
    std::vector<Item> items;
    QStackedLayout layout;
    QHBoxLayout topBar;
    QWidget *topWidget = nullptr;
    QWidget *horizontalSeparator = nullptr;
};

}  // namespace chatterino

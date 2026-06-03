// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/ComboBoxItemDelegate.hpp"

#include <QComboBox>

namespace chatterino {

ComboBoxItemDelegate::ComboBoxItemDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

ComboBoxItemDelegate::~ComboBoxItemDelegate()
{
}

QWidget *ComboBoxItemDelegate::createEditor(QWidget *parent,
                                            const QStyleOptionViewItem &option,
                                            const QModelIndex &index) const
{
    QVariant data = index.data(Qt::UserRole + 1);

    if (data.metaType() != QMetaType::fromType<QStringList>())
    {
        return QStyledItemDelegate::createEditor(parent, option, index);
    }

    QComboBox *combo = new QComboBox(parent);
    combo->addItems(data.toStringList());
    return combo;
}

void ComboBoxItemDelegate::setEditorData(QWidget *editor,
                                         const QModelIndex &index) const
{
    if (QComboBox *cb = qobject_cast<QComboBox *>(editor))
    {

        QString currentText = index.data(Qt::EditRole).toString();
        int cbIndex = cb->findText(currentText);

        if (cbIndex >= 0)
        {
            cb->setCurrentIndex(cbIndex);
        }
    }
    else
    {
        QStyledItemDelegate::setEditorData(editor, index);
    }
}

void ComboBoxItemDelegate::setModelData(QWidget *editor,
                                        QAbstractItemModel *model,
                                        const QModelIndex &index) const
{
    if (QComboBox *cb = qobject_cast<QComboBox *>(editor))
    {

        model->setData(index, cb->currentText(), Qt::EditRole);
    }
    else
    {
        QStyledItemDelegate::setModelData(editor, model, index);
    }
}
}

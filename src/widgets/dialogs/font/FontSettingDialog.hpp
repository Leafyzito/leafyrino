// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/ChatterinoSetting.hpp"
#include "widgets/dialogs/font/FontDialog.hpp"

#include <QObject>

namespace chatterino {

class FontSettingDialog : public FontDialog
{
    Q_OBJECT

public:
    FontSettingDialog(QStringSetting &family, IntSetting &size,
                      IntSetting &weight, QWidget *parent = nullptr);

private:
    void setSettings();

    void restoreSettings();

    QStringSetting &familySetting;
    IntSetting &sizeSetting;
    IntSetting &weightSetting;

    QString oldFamily;
    int oldSize;
    int oldWeight;
};

}  // namespace chatterino

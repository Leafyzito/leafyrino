// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/ChatterinoSetting.hpp"
#include "util/SignalListener.hpp"

#include <QLabel>
#include <QWidget>

namespace chatterino {

class FontSettingDialog;

class FontSettingWidget : public QWidget
{
public:
    FontSettingWidget(QStringSetting &family, IntSetting &size,
                      IntSetting &weight, QWidget *parent = nullptr);

private:
    void updateCurrentLabel();
    void showDialog();

    FontSettingDialog *dialog = nullptr;

    QStringSetting &familySetting;
    IntSetting &sizeSetting;
    IntSetting &weightSetting;

    QLabel *currentLabel;
    SignalListener listener;
};

}  // namespace chatterino

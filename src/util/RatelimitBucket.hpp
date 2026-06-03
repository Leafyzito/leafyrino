// SPDX-FileCopyrightText: 2021 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QList>
#include <QObject>
#include <QString>

#include <functional>

namespace chatterino {

class RatelimitBucket : public QObject
{
public:
    RatelimitBucket(int budget, int cooldown,
                    std::function<void(QString)> callback, QObject *parent);

    void send(QString channel);

private:

    int budget_;

    const int cooldown_;

    std::function<void(QString)> callback_;
    QList<QString> queue_;

    void handleOne();
};

}

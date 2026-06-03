// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QStringList>

namespace chatterino {

const QStringList &getSampleCheerMessages();
const QStringList &getSampleSubMessages();
const QStringList &getSampleMiscMessages();
const QStringList &getSampleEmoteTestMessages();

QByteArray getSampleChannelRewardMessage();
QByteArray getSampleChannelRewardMessage2();
const QString &getSampleChannelRewardIRCMessage();

const QStringList &getSampleLinkMessages();

}  // namespace chatterino

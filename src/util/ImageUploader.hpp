// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/Expected.hpp"

class QJsonObject;
class QString;

namespace chatterino {

class Settings;

namespace imageuploader::detail {

QJsonObject exportSettings(const Settings &s);

bool importSettings(const QJsonObject &settingsObj, Settings &s);

ExpectedStr<QJsonObject> validateImportJson(const QString &clipboardText);

}

}

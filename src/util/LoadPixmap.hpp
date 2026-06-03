// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once
#include "common/Aliases.hpp"

#include <QPixmap>

namespace chatterino {

void loadPixmapFromUrl(const Url &url, std::function<void(QPixmap)> &&callback);

}

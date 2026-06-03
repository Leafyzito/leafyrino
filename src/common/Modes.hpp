// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

namespace chatterino {

class Modes
{
public:
    Modes();

    static const Modes &instance();

    bool isPortable{};

    bool isExternallyPackaged{};
};

}  // namespace chatterino

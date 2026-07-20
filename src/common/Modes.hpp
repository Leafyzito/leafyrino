// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

namespace chatterino {

class Args;

class Modes
{
public:
    explicit Modes(const Args &args);

    /// Marked by the line `portable` or `portableEnable` from `Args`
    bool isPortable{};

    bool isExternallyPackaged{};
};

}  // namespace chatterino

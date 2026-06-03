// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QNetworkProxy>

namespace chatterino {

class Env;

class NetworkConfigurationProvider
{
public:
    NetworkConfigurationProvider() = delete;

    static void applyFromEnv(const Env &env);
};

}  // namespace chatterino

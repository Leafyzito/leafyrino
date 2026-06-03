// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/sound/ISoundController.hpp"

namespace chatterino {

class NullBackend final : public ISoundController
{
public:
    NullBackend();
    ~NullBackend() override = default;
    NullBackend(const NullBackend &) = delete;
    NullBackend(NullBackend &&) = delete;
    NullBackend &operator=(const NullBackend &) = delete;
    NullBackend &operator=(NullBackend &&) = delete;

    void play(const QUrl &sound) final;
};

}

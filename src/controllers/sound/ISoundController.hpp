// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QUrl>

namespace chatterino {

enum class SoundBackend {
    Miniaudio,
    Null,
};

class ISoundController
{
public:
    ISoundController() = default;
    virtual ~ISoundController() = default;
    ISoundController(const ISoundController &) = delete;
    ISoundController(ISoundController &&) = delete;
    ISoundController &operator=(const ISoundController &) = delete;
    ISoundController &operator=(ISoundController &&) = delete;

    virtual void play(const QUrl &sound) = 0;
};

}

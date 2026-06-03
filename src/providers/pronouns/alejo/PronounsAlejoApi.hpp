// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/pronouns/UserPronouns.hpp"

#include <QJsonObject>
#include <QString>

#include <atomic>
#include <functional>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

namespace chatterino::pronouns {

class AlejoApi
{
public:
    AlejoApi();

    void fetch(const QString &username,
               const std::function<void(std::optional<UserPronouns>)> &onDone);

private:
    void loadAvailablePronouns();
    void scheduleAvailablePronounsRetry();

    std::shared_mutex mutex;

    std::unordered_map<QString, QString> pronouns;
    std::atomic_bool pronounsLoadInFlight_{false};
    std::atomic_int pronounsLoadRetryCount_{0};

    UserPronouns parsePronoun(const QJsonObject &object);
};

}

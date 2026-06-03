// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/pronouns/alejo/PronounsAlejoApi.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "providers/pronouns/UserPronouns.hpp"
#include "util/PostToThread.hpp"

#include <QStringBuilder>
#include <QTimer>

#include <mutex>
#include <unordered_map>

namespace {

const auto &LOG = chatterinoPronouns;

constexpr QStringView API_URL = u"https://api.pronouns.alejo.io/v1";
constexpr QStringView API_USERS_ENDPOINT = u"/users";
constexpr QStringView API_PRONOUNS_ENDPOINT = u"/pronouns";
constexpr int API_TIMEOUT_MS = 5 * 1000;
constexpr int MAX_PRONOUN_LIST_RETRIES = 5;

}  // namespace

namespace chatterino::pronouns {

AlejoApi::AlejoApi()
{
    this->loadAvailablePronouns();
}

void AlejoApi::fetch(
    const QString &username,
    const std::function<void(std::optional<UserPronouns>)> &onDone)
{
    {
        std::shared_lock lock(this->mutex);
        if (this->pronouns.empty())
        {
            this->loadAvailablePronouns();
            runInGuiThread([this, username, onDone] {
                QTimer::singleShot(750, [this, username, onDone] {
                    if (isAppAboutToQuit())
                    {
                        return;
                    }

                    {
                        std::shared_lock retryLock(this->mutex);
                        if (this->pronouns.empty())
                        {
                            onDone({});
                            return;
                        }
                    }
                    this->fetch(username, onDone);
                });
            });
            return;
        }
    }

    qCDebug(LOG) << "Fetching pronouns from alejo.io for" << username;

    QString endpoint = API_URL % API_USERS_ENDPOINT % "/" % username;

    NetworkRequest(endpoint)
        .timeout(API_TIMEOUT_MS)
        .concurrent()
        .onSuccess([this, username, onDone](const auto &result) {
            auto object = result.parseJson();
            auto parsed = this->parsePronoun(object);
            onDone({parsed});
        })
        .onError([onDone, username](auto result) {
            auto status = result.status();
            if (status.has_value() && status == 404)
            {
                onDone({UserPronouns()});
                return;
            }
            qCWarning(LOG) << "alejo.io returned " << status.value_or(-1)
                           << " when fetching pronouns for " << username;
            onDone({});
        })
        .execute();
}

void AlejoApi::loadAvailablePronouns()
{
    if (this->pronounsLoadInFlight_.exchange(true))
    {
        return;
    }

    qCDebug(LOG) << "Fetching available pronouns for alejo.io";

    QString endpoint = API_URL % API_PRONOUNS_ENDPOINT;

    NetworkRequest(endpoint)
        .timeout(API_TIMEOUT_MS)
        .concurrent()
        .onSuccess([this](const auto &result) {
            auto root = result.parseJson();
            if (root.isEmpty())
            {
                this->pronounsLoadInFlight_ = false;
                this->scheduleAvailablePronounsRetry();
                return;
            }

            std::unordered_map<QString, QString> newPronouns;

            for (auto it = root.begin(); it != root.end(); ++it)
            {
                const auto &pronounId = it.key();
                const auto &pronounObj = it.value().toObject();

                const auto &subject = pronounObj["subject"].toString();
                const auto &object = pronounObj["object"].toString();
                const auto &singular = pronounObj["singular"].toBool();

                if (subject.isEmpty() || object.isEmpty())
                {
                    qCWarning(LOG) << "Pronoun" << pronounId
                                   << "was malformed:" << pronounObj;
                    continue;
                }

                if (singular)
                {
                    newPronouns[pronounId] = subject;
                }
                else
                {
                    newPronouns[pronounId] = subject % "/" % object;
                }
            }

            {
                std::unique_lock lock(this->mutex);
                this->pronouns = newPronouns;
            }
            this->pronounsLoadRetryCount_ = 0;
            this->pronounsLoadInFlight_ = false;
        })
        .onError([this](const NetworkResult &result) {
            qCWarning(LOG) << "Failed to load pronouns from alejo.io"
                           << result.formatError();
            this->pronounsLoadInFlight_ = false;
            this->scheduleAvailablePronounsRetry();
        })
        .execute();
}

void AlejoApi::scheduleAvailablePronounsRetry()
{
    const auto retryCount = this->pronounsLoadRetryCount_.fetch_add(1);
    if (retryCount >= MAX_PRONOUN_LIST_RETRIES)
    {
        return;
    }

    const auto delayMs = (retryCount + 1) * 5000;
    runInGuiThread([this, delayMs] {
        QTimer::singleShot(delayMs, [this] {
            if (isAppAboutToQuit())
            {
                return;
            }

            {
                std::shared_lock lock(this->mutex);
                if (!this->pronouns.empty())
                {
                    return;
                }
            }
            this->loadAvailablePronouns();
        });
    });
}

UserPronouns AlejoApi::parsePronoun(const QJsonObject &object)
{
    std::shared_lock lock(this->mutex);
    if (this->pronouns.empty())
    {
        return {};
    }

    const auto &pronoun = object["pronoun_id"];

    if (!pronoun.isString())
    {
        return {};
    }

    auto pronounStr = pronoun.toString();
    auto iter = this->pronouns.find(pronounStr);
    if (iter != this->pronouns.end())
    {
        return {iter->second};
    }
    return {};
}

}  // namespace chatterino::pronouns

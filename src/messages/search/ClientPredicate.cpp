// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/search/ClientPredicate.hpp"

#include <algorithm>
#include <optional>

namespace chatterino {

namespace {

using Status = Message::ClientDetectionStatus;

std::optional<Status> parseClient(const QString &client)
{
    const auto name = client.toLower();
    if (name == "web" || name == "webchat")
    {
        return Status::Web;
    }
    if (name == "android")
    {
        return Status::Android;
    }
    if (name == "ios" || name == "iphone")
    {
        return Status::IOS;
    }
    if (name == "mobile")
    {
        return Status::MobileNew;
    }
    if (name == "abnormal")
    {
        return Status::Abnormal;
    }
    if (name == "unknown")
    {
        return Status::Unknown;
    }
    return std::nullopt;
}

}  // namespace

ClientPredicate::ClientPredicate(const QString &clients, bool negate)
    : MessagePredicate(negate)
{
    for (const auto &client : clients.split(',', Qt::SkipEmptyParts))
    {
        if (auto status = parseClient(client))
        {
            this->clients_.push_back(*status);
        }
    }
}

bool ClientPredicate::appliesToImpl(const Message &message)
{
    return std::ranges::find(this->clients_, message.clientDetection) !=
           this->clients_.end();
}

}  // namespace chatterino

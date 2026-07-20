// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/ChatterinoSetting.hpp"
#include "common/SignalVector.hpp"
#include "util/Expected.hpp"
#include "util/QStringHash.hpp"
#include "util/RapidJsonSerializeQString.hpp"

#include <boost/signals2.hpp>
#include <QString>

#include <memory>
#include <mutex>
#include <vector>

namespace chatterino {

class TwitchAccount;
class AccountController;

extern const std::vector<QStringView> AUTH_SCOPES;

class TwitchAccountManager
{
    TwitchAccountManager();

public:
    struct UserData {
        QString username;
        QString userID;
        QString clientID;
        QString oauthToken;
    };

    std::shared_ptr<TwitchAccount> getCurrent();

    std::vector<QString> getUsernames() const;

    std::shared_ptr<TwitchAccount> findUserByUsername(
        const QString &username) const;
    bool userExists(const QString &username) const;

    void reloadUsers();
    void load();

    bool isLoggedIn() const;

    pajlada::Settings::Setting<QString> currentUsername{"/accounts/current",
                                                        ""};

    pajlada::Signals::Signal<std::shared_ptr<TwitchAccount>,
                             std::shared_ptr<TwitchAccount>>
        currentUserAboutToChange;

    boost::signals2::signal<void()> currentUserChanged;
    pajlada::Signals::NoArgSignal userListUpdated;
    pajlada::Signals::NoArgSignal currentUserNameChanged;

    /// Fired when the IRC server sends a "Login authentication failed" notice,
    /// indicating the current OAuth token has expired.
    pajlada::Signals::NoArgSignal loginExpired;

    SignalVector<std::shared_ptr<TwitchAccount>> accounts;

    pajlada::Signals::Signal<void *, ExpectedStr<void>> emotesReloaded;

private:
    enum class AddUserResponse {
        UserAlreadyExists,
        UserValuesUpdated,
        UserAdded,
    };
    AddUserResponse addUser(const UserData &data);
    bool removeUser(TwitchAccount *account);

    std::shared_ptr<TwitchAccount> currentUser_;

    std::shared_ptr<TwitchAccount> anonymousUser_;
    mutable std::mutex mutex_;

    friend class AccountController;
};

}  // namespace chatterino

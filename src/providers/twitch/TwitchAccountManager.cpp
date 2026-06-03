// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/TwitchAccountManager.hpp"

#include "Application.hpp"
#include "common/Args.hpp"
#include "common/Common.hpp"
#include "common/Literals.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/twitch/TwitchUser.hpp"
#include "singletons/Settings.hpp"
#include "util/QCompareTransparent.hpp"
#include "util/SharedPtrElementLess.hpp"

#include <QStringBuilder>

namespace {

using namespace chatterino;
using namespace literals;

QString missingScopes(const QJsonArray &scopesArray)
{
    std::set<QString, QCompareTransparent> scopes;
    for (auto s : scopesArray)
    {
        scopes.emplace(s.toString());
    }

    QString missingList;
    for (auto scope : AUTH_SCOPES)
    {
        if (!scopes.contains(scope))
        {
            if (!missingList.isEmpty())
            {
                missingList.append(u", ");
            }
            missingList.append(scope);
        }
    }

    return missingList;
}

void checkMissingScopes(const std::shared_ptr<TwitchAccount> &account)
{
    NetworkRequest(u"https://id.twitch.tv/oauth2/validate"_s,
                   NetworkRequestType::Get)
        .header("Authorization", u"OAuth " % account->getOAuthToken())
        .timeout(20000)
        .onSuccess([account](const auto &res) {
            auto *app = tryGetApp();
            if (!app)
            {
                return;
            }

            const auto json = res.parseJson();

            const auto login = json["login"_L1].toString();
            if (!login.isEmpty() &&
                login.compare(account->getUserName(), Qt::CaseInsensitive) != 0)
            {
                account->setUserName(login);
                const std::string basePath =
                    "/accounts/uid" + account->getUserId().toStdString();
                pajlada::Settings::Setting<QString>::set(basePath + "/username",
                                                         login);
                auto &manager = app->getAccounts()->twitch;
                auto currentUsername = manager.getCurrent()->getUserName();
                if (currentUsername.compare(manager.currentUsername.getValue(),
                                            Qt::CaseInsensitive) != 0)
                {
                    manager.currentUsername = currentUsername;
                }
                getSettings()->requestSave();
                app->getAccounts()->twitch.currentUserNameChanged.invoke();
            }

            auto missing = missingScopes(json["scopes"_L1].toArray());
            if (missing.isEmpty())
            {
                return;
            }

            auto msg = MessageBuilder::makeMissingScopesMessage(missing);
            app->getTwitch()->forEachChannel([msg](const auto &chan) {
                chan->addMessage(msg, MessageContext::Original);
            });
        })
        .onError([](const auto &res) {
            qCWarning(chatterinoTwitch)
                << "Failed to check for missing scopes:" << res.formatError();
        })
        .execute();
}

}

namespace chatterino {

const std::vector<QStringView> AUTH_SCOPES{
    u"channel:moderate",
    u"channel:read:redemptions",
    u"chat:edit",
    u"chat:read",
    u"whispers:read",

    u"channel:edit:commercial",

    u"clips:edit",

    u"channel:manage:broadcast",

    u"user:read:blocked_users",

    u"user:manage:blocked_users",

    u"moderator:manage:automod",

    u"channel:manage:raids",

    u"channel:manage:polls",

    u"channel:read:polls",

    u"channel:manage:predictions",

    u"channel:read:predictions",

    u"moderator:manage:announcements",

    u"user:manage:whispers",

    u"moderator:manage:banned_users",

    u"moderator:manage:chat_messages",

    u"user:manage:chat_color",

    u"moderator:manage:chat_settings",

    u"channel:manage:moderators",

    u"channel:manage:vips",

    u"moderator:read:chatters",

    u"moderator:manage:shield_mode",

    u"moderator:manage:shoutouts",

    u"user:read:moderated_channels",

    u"user:read:chat",

    u"user:write:chat",

    u"user:read:emotes",

    u"moderator:manage:warnings",

    u"user:read:follows",

    u"moderator:manage:blocked_terms",

    u"moderator:manage:unban_requests",

    u"moderator:read:moderators",

    u"moderator:read:vips",

    u"moderator:read:suspicious_users",

    u"moderator:manage:suspicious_users",
};

TwitchAccountManager::TwitchAccountManager()
    : accounts(SharedPtrElementLess<TwitchAccount>{})
    , anonymousUser_(new TwitchAccount(ANONYMOUS_USERNAME, "", "", ""))
{
    this->currentUserChanged.connect([this] {
        auto currentUser = this->getCurrent();
        currentUser->loadBlocks();
        currentUser->loadSeventvUserID();
        if (!currentUser->isAnon())
        {
            checkMissingScopes(currentUser);
        }
    });

    std::ignore = this->accounts.itemRemoved.connect([this](const auto &acc) {
        this->removeUser(acc.item.get());
    });
}

std::shared_ptr<TwitchAccount> TwitchAccountManager::getCurrent()
{
    if (!this->currentUser_)
    {
        return this->anonymousUser_;
    }

    return this->currentUser_;
}

std::vector<QString> TwitchAccountManager::getUsernames() const
{
    std::vector<QString> userNames;

    std::lock_guard<std::mutex> lock(this->mutex_);

    for (const auto &user : this->accounts)
    {
        userNames.push_back(user->getUserName());
    }

    return userNames;
}

std::shared_ptr<TwitchAccount> TwitchAccountManager::findUserByUsername(
    const QString &username) const
{
    std::lock_guard<std::mutex> lock(this->mutex_);

    for (const auto &user : this->accounts)
    {
        if (username.compare(user->getUserName(), Qt::CaseInsensitive) == 0)
        {
            return user;
        }
    }

    return nullptr;
}

bool TwitchAccountManager::userExists(const QString &username) const
{
    return this->findUserByUsername(username) != nullptr;
}

void TwitchAccountManager::reloadUsers()
{
    auto keys = pajlada::Settings::SettingManager::getObjectKeys("/accounts");

    UserData userData;

    bool listUpdated = false;

    for (const auto &uid : keys)
    {
        if (uid == "current")
        {
            continue;
        }

        auto username = pajlada::Settings::Setting<QString>::get(
            "/accounts/" + uid + "/username");
        auto userID = pajlada::Settings::Setting<QString>::get("/accounts/" +
                                                               uid + "/userID");
        auto clientID = pajlada::Settings::Setting<QString>::get(
            "/accounts/" + uid + "/clientID");
        auto oauthToken = pajlada::Settings::Setting<QString>::get(
            "/accounts/" + uid + "/oauthToken");

        if (username.isEmpty() || userID.isEmpty() || clientID.isEmpty() ||
            oauthToken.isEmpty())
        {
            continue;
        }

        userData.username = username.trimmed();
        userData.userID = userID.trimmed();
        userData.clientID = clientID.trimmed();
        userData.oauthToken = oauthToken.trimmed();

        switch (this->addUser(userData))
        {
            case AddUserResponse::UserAlreadyExists: {
                qCDebug(chatterinoTwitch)
                    << "User" << userData.username << "already exists";

            }
            break;
            case AddUserResponse::UserValuesUpdated: {
                qCDebug(chatterinoTwitch)
                    << "User" << userData.username
                    << "already exists, and values updated!";
                if (userData.username == this->getCurrent()->getUserName())
                {
                    qCDebug(chatterinoTwitch)
                        << "It was the current user, so we need to "
                           "reconnect stuff!";
                    this->currentUserChanged();
                }
            }
            break;
            case AddUserResponse::UserAdded: {
                qCDebug(chatterinoTwitch) << "Added user" << userData.username;
                listUpdated = true;
            }
            break;
        }
    }

    if (listUpdated)
    {
        this->userListUpdated.invoke();
    }
}

void TwitchAccountManager::load()
{
    if (getApp()->getArgs().initialLogin.has_value())
    {
        this->currentUsername = getApp()->getArgs().initialLogin.value();
    }

    this->reloadUsers();

    this->currentUsername.connect([this](const QString &newUsername) {
        auto user = this->findUserByUsername(newUsername);

        this->currentUserAboutToChange.invoke(this->currentUser_, user);

        if (user)
        {
            qCDebug(chatterinoTwitch)
                << "Twitch user updated to" << newUsername;
            getHelix()->update(user->getOAuthClient(), user->getOAuthToken());
            this->currentUser_ = user;
        }
        else
        {
            qCDebug(chatterinoTwitch) << "Twitch user updated to anonymous";
            this->currentUser_ = this->anonymousUser_;
        }

        this->currentUserChanged();
        this->currentUser_->reloadEmotes();
    });
}

bool TwitchAccountManager::isLoggedIn() const
{
    if (!this->currentUser_)
    {
        return false;
    }

    return !this->currentUser_->isAnon();
}

bool TwitchAccountManager::removeUser(TwitchAccount *account)
{
    static const QString accountFormat("/accounts/uid%1");

    auto userID(account->getUserId());
    if (!userID.isEmpty())
    {
        pajlada::Settings::SettingManager::gRemoveSetting(
            accountFormat.arg(userID).toStdString());
    }

    if (account->getUserName() == this->currentUsername)
    {

        this->currentUsername = "";
    }

    this->userListUpdated.invoke();

    return true;
}

TwitchAccountManager::AddUserResponse TwitchAccountManager::addUser(
    const TwitchAccountManager::UserData &userData)
{
    auto previousUser = this->findUserByUsername(userData.username);
    if (previousUser)
    {
        bool userUpdated = false;

        if (previousUser->setOAuthClient(userData.clientID))
        {
            userUpdated = true;
        }

        if (previousUser->setOAuthToken(userData.oauthToken))
        {
            userUpdated = true;
        }

        if (userUpdated)
        {
            return AddUserResponse::UserValuesUpdated;
        }
        else
        {
            return AddUserResponse::UserAlreadyExists;
        }
    }

    auto newUser =
        std::make_shared<TwitchAccount>(userData.username, userData.oauthToken,
                                        userData.clientID, userData.userID);

    this->accounts.insert(newUser);

    return AddUserResponse::UserAdded;
}

}

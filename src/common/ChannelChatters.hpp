// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/ChatterSet.hpp"
#include "common/UniqueAccess.hpp"
#include "lrucache/lrucache.hpp"
#include "messages/MessageElement.hpp"
#include "util/QStringHash.hpp"

#include <QColor>
#include <QObject>
#include <QRgb>

namespace chatterino {

class Channel;

class ChannelChatters
{
public:
    ChannelChatters(Channel &channel);
    virtual ~ChannelChatters() = default;

    SharedAccessGuard<const ChatterSet> accessChatters() const;

    void addRecentChatter(const QString &user);
    void addJoinedUser(const QString &user, bool isMod, bool isBroadcaster);
    void addPartedUser(const QString &user, bool isMod, bool isBroadcaster);
    QColor getUserColor(const QString &user) const;
    void setUserColor(const QString &user, const QColor &color);
    void updateOnlineChatters(const std::unordered_set<QString> &usernames);

    size_t colorsSize() const;

    MessageElementFlag mentionFlag() const
    {
        return this->mentionFlags_;
    }

    void setMentionFlag(MessageElementFlag flag);

    static constexpr int maxChatterColorCount = 5000;

private:
    Channel &channel_;

    UniqueAccess<ChatterSet> chatters_;
    UniqueAccess<cache::lru_cache<QString, QRgb>> chatterColors_;

    UniqueAccess<QStringList> joinedUsers_;
    bool joinedUsersMergeQueued_ = false;
    UniqueAccess<QStringList> partedUsers_;
    bool partedUsersMergeQueued_ = false;

    MessageElementFlag mentionFlags_ = MessageElementFlag::None;

    QObject lifetimeGuard_;
};

}  // namespace chatterino

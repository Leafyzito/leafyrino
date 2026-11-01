// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

namespace chatterino {

struct Link {
public:
    enum Type {
        None,
        Url,
        UserInfo,
        UserWhisper,
        InsertText,
        UserAction,
        AutoModAllow,
        AutoModDeny,
        OpenAccountsPage,
        JumpToChannel,
        JumpToOrCreateChannel,
        Reconnect,
        CopyToClipboard,
        ReplyToMessage,
        ViewThread,
        JumpToMessage,
        AcknowledgeChatWarning,
        /// value is the review key
        AutoModReviewApprove,
        /// value is the review key
        AutoModReviewDeny,
        /// value is "<review key>\n<seconds>", or just the review key to ask
        /// for a duration
        AutoModReviewTimeout,
        /// value is the review key
        AutoModReviewBan,
        /// value is the review key
        AutoModReviewRetry,
    };

    Link();
    Link(Type getType, const QString &getValue);

    Type type;
    QString value;

    bool isValid() const;
    bool isUrl() const;
};

}  // namespace chatterino

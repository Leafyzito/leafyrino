// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Image.hpp"

namespace chatterino {

class LinkInfo : public QObject
{
    Q_OBJECT

public:
    enum class State {

        Created,

        Loading,

        Resolved,

        Errored,
    };

    [[nodiscard]] explicit LinkInfo(QString url);

    ~LinkInfo() override;

    LinkInfo(const LinkInfo &) = delete;
    LinkInfo(LinkInfo &&) = delete;
    LinkInfo &operator=(const LinkInfo &) = delete;
    LinkInfo &operator=(LinkInfo &&) = delete;

    [[nodiscard]] QString url() const;

    [[nodiscard]] QString originalUrl() const;

    [[nodiscard]] State state() const;

    [[nodiscard]] bool isPending() const;

    [[nodiscard]] bool isLoading() const;

    [[nodiscard]] bool isLoaded() const;

    [[nodiscard]] bool isResolved() const;

    [[nodiscard]] bool hasError() const;

    [[nodiscard]] bool hasThumbnail() const;

    [[nodiscard]] QString tooltip() const;

    [[nodiscard]] ImagePtr thumbnail() const;

    void setState(State state);

    void setResolvedUrl(QString resolvedUrl);

    void setTooltip(QString tooltip);

    void setThumbnail(ImagePtr thumbnail);

Q_SIGNALS:

    void stateChanged(State state);

private:
    const QString originalUrl_;
    QString url_;

    QString tooltip_;
    ImagePtr thumbnail_;

    State state_ = State::Created;
};

}  // namespace chatterino

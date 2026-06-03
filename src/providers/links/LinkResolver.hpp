// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

namespace chatterino {

class LinkInfo;

class ILinkResolver
{
public:
    ILinkResolver() = default;
    virtual ~ILinkResolver() = default;
    ILinkResolver(const ILinkResolver &) = delete;
    ILinkResolver(ILinkResolver &&) = delete;
    ILinkResolver &operator=(const ILinkResolver &) = delete;
    ILinkResolver &operator=(ILinkResolver &&) = delete;

    virtual void resolve(LinkInfo *info) = 0;
};

class LinkResolver : public ILinkResolver
{
public:
    LinkResolver() = default;

    void resolve(LinkInfo *info) override;
};

}

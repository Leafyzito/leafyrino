// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <boost/asio/ip/basic_resolver_results.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <optional>
#include <vector>

namespace chatterino::ws::detail {

class BalancedResolverResults
{
public:
    using Protocol = boost::asio::ip::tcp;
    using Entry = boost::asio::ip::basic_resolver_entry<Protocol>;

    BalancedResolverResults() = default;
    explicit BalancedResolverResults(
        const Protocol::resolver::results_type &results);
    explicit BalancedResolverResults(std::vector<Entry> entries);

    std::optional<Entry> advanceEntry();

    std::optional<Entry> currentEntry() const;

    void reset();

private:

    bool advanceIPv4();

    bool advanceIPv6();

    size_t nextIPv4Idx = 0;
    size_t nextIPv6Idx = 0;
    bool nextIsIPv6 = true;

    size_t currentIdx = std::numeric_limits<size_t>::max();

    std::vector<Entry> entries;
};

}

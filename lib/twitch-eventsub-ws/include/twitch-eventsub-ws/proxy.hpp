#pragma once

#include <boost/beast/core/tcp_stream.hpp>
#include <boost/system/error_category.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace chatterino::eventsub::lib {

struct ProxyConfig {
    enum class Type : std::uint8_t {
        Http,
        Socks5,
    };

    Type type = Type::Http;
    std::string host;
    std::uint16_t port = 0;
    std::string username;
    std::string password;

    bool hasCredentials() const
    {
        return !this->username.empty() || !this->password.empty();
    }
};

namespace proxy {

// NOLINTNEXTLINE(performance-enum-size)
enum class Error : int {
    InvalidResponse = 1,
    UnexpectedData,
    NoAcceptableAuthMethod,
    AuthFailed,
    HostTooLong,
    CredentialsTooLong,

    /// HTTP proxies: HttpStatus + the status code of a non-2xx response
    HttpStatus = 1000,
    /// SOCKS5 proxies: Socks5Reply + the REP field of a failed reply
    Socks5Reply = 2000,
};

class ProxyErrorCategory final : public boost::system::error_category
{
public:
    consteval ProxyErrorCategory() = default;

    const char *name() const noexcept override
    {
        return "Proxy error";
    }

    std::string message(int ev) const override;
};

inline constexpr ProxyErrorCategory CATEGORY;

inline boost::system::error_code makeCode(Error kind, int offset = 0)
{
    return {static_cast<int>(kind) + offset, CATEGORY};
}

}  // namespace proxy

using ProxyHandshakeHandler = std::function<void(boost::system::error_code)>;

/// Asks the proxy that `stream` is connected to for a tunnel to
/// `targetHost`:`targetPort`.
///
/// The target host is passed to the proxy as-is, so it's resolved by the proxy
/// and not locally. `handler` is invoked on the stream's executor once the
/// tunnel is ready (or failed). After a successful handshake, `stream` can be
/// used as if it were connected to the target directly.
///
/// The caller must keep `stream` alive until `handler` is invoked and is
/// responsible for any timeouts (e.g. `stream.expires_after`).
void asyncProxyHandshake(boost::beast::tcp_stream &stream,
                         const ProxyConfig &proxy, std::string targetHost,
                         std::uint16_t targetPort,
                         ProxyHandshakeHandler handler);

}  // namespace chatterino::eventsub::lib

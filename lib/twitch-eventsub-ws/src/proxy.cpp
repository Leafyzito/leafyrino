#include "twitch-eventsub-ws/proxy.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/detail/base64.hpp>

#include <array>
#include <cassert>
#include <charconv>
#include <memory>
#include <string_view>

namespace chatterino::eventsub::lib {

namespace proxy {

std::string ProxyErrorCategory::message(int ev) const
{
    using namespace std::string_literals;

    if (ev >= static_cast<int>(Error::Socks5Reply))
    {
        // RFC 1928, section 6
        switch (ev - static_cast<int>(Error::Socks5Reply))
        {
            case 1:
                return "SOCKS5: general SOCKS server failure"s;
            case 2:
                return "SOCKS5: connection not allowed by ruleset"s;
            case 3:
                return "SOCKS5: network unreachable"s;
            case 4:
                return "SOCKS5: host unreachable"s;
            case 5:
                return "SOCKS5: connection refused"s;
            case 6:
                return "SOCKS5: TTL expired"s;
            case 7:
                return "SOCKS5: command not supported"s;
            case 8:
                return "SOCKS5: address type not supported"s;
            default:
                return "SOCKS5: unknown reply "s +
                       std::to_string(ev -
                                      static_cast<int>(Error::Socks5Reply));
        }
    }
    if (ev >= static_cast<int>(Error::HttpStatus))
    {
        return "HTTP proxy responded with status "s +
               std::to_string(ev - static_cast<int>(Error::HttpStatus));
    }

    switch (static_cast<Error>(ev))
    {
        case Error::InvalidResponse:
            return "Invalid response from proxy"s;
        case Error::UnexpectedData:
            return "Proxy sent unexpected data after the handshake"s;
        case Error::NoAcceptableAuthMethod:
            return "Proxy didn't accept any of our authentication methods"s;
        case Error::AuthFailed:
            return "Proxy rejected the username or password"s;
        case Error::HostTooLong:
            return "Target host name is too long for the proxy"s;
        case Error::CredentialsTooLong:
            return "Proxy username or password is too long"s;
        default:
            break;
    }

    assert(false);
    return "Unknown error"s;
}

}  // namespace proxy

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
using boost::system::error_code;

// RFC 1928 and RFC 1929
constexpr std::uint8_t SOCKS_VERSION = 0x05;
constexpr std::uint8_t SOCKS_AUTH_NONE = 0x00;
constexpr std::uint8_t SOCKS_AUTH_PASSWORD = 0x02;
constexpr std::uint8_t SOCKS_AUTH_UNACCEPTABLE = 0xff;
constexpr std::uint8_t SOCKS_PASSWORD_VERSION = 0x01;
constexpr std::uint8_t SOCKS_CMD_CONNECT = 0x01;
constexpr std::uint8_t SOCKS_ATYP_IPV4 = 0x01;
constexpr std::uint8_t SOCKS_ATYP_DOMAIN = 0x03;
constexpr std::uint8_t SOCKS_ATYP_IPV6 = 0x04;

constexpr std::size_t MAX_HTTP_RESPONSE_SIZE = 16 * 1024;

std::string base64(std::string_view in)
{
    std::string out(beast::detail::base64::encoded_size(in.size()), '\0');
    out.resize(beast::detail::base64::encode(out.data(), in.data(), in.size()));
    return out;
}

class HandshakeOp : public std::enable_shared_from_this<HandshakeOp>
{
public:
    HandshakeOp(beast::tcp_stream &stream, const ProxyConfig &proxy,
                std::string host, std::uint16_t port,
                ProxyHandshakeHandler handler)
        : stream(stream)
        , proxy(proxy)
        , host(std::move(host))
        , port(port)
        , handler(std::move(handler))
    {
    }

    void start()
    {
        switch (this->proxy.type)
        {
            case ProxyConfig::Type::Http:
                this->httpConnect();
                break;
            case ProxyConfig::Type::Socks5:
                this->socksGreet();
                break;
        }
    }

private:
    using Step = void (HandshakeOp::*)();

    void writeOut(Step next)
    {
        asio::async_write(
            this->stream, asio::buffer(this->out),
            [self{this->shared_from_this()}, next](error_code ec, size_t) {
                if (ec)
                {
                    self->finish(ec);
                    return;
                }
                ((*self).*next)();
            });
    }

    void readIn(std::size_t n, Step next)
    {
        assert(n <= this->buf.size());
        asio::async_read(
            this->stream, asio::buffer(this->buf.data(), n),
            [self{this->shared_from_this()}, next](error_code ec, size_t) {
                if (ec)
                {
                    self->finish(ec);
                    return;
                }
                ((*self).*next)();
            });
    }

    void finish(error_code ec)
    {
        if (!this->handler)
        {
            return;
        }
        auto handler = std::move(this->handler);
        this->handler = nullptr;
        handler(ec);
    }

    void fail(proxy::Error kind, int offset = 0)
    {
        this->finish(proxy::makeCode(kind, offset));
    }

    // MARK: HTTP

    void httpConnect()
    {
        // IPv6 literals need brackets in the authority
        auto authority = this->host.find(':') != std::string::npos
                             ? '[' + this->host + ']'
                             : this->host;
        authority += ':' + std::to_string(this->port);

        this->out = "CONNECT " + authority + " HTTP/1.1\r\n";
        this->out += "Host: " + authority + "\r\n";
        if (this->proxy.hasCredentials())
        {
            this->out +=
                "Proxy-Authorization: Basic " +
                base64(this->proxy.username + ':' + this->proxy.password) +
                "\r\n";
        }
        this->out += "\r\n";

        this->writeOut(&HandshakeOp::httpReadResponse);
    }

    void httpReadResponse()
    {
        asio::async_read_until(
            this->stream,
            asio::dynamic_buffer(this->response, MAX_HTTP_RESPONSE_SIZE),
            "\r\n\r\n",
            [self{this->shared_from_this()}](error_code ec, size_t n) {
                self->onHttpResponse(ec, n);
            });
    }

    void onHttpResponse(error_code ec, std::size_t headerSize)
    {
        if (ec)
        {
            this->finish(ec);
            return;
        }

        // anything past the header would already belong to the tunnel, but
        // we only ever talk first (TLS/WS handshake), so this shouldn't happen
        if (this->response.size() != headerSize)
        {
            this->fail(proxy::Error::UnexpectedData);
            return;
        }

        // HTTP/1.x SP status-code SP reason-phrase
        std::string_view line(this->response);
        line = line.substr(0, line.find("\r\n"));
        if (!line.starts_with("HTTP/1.") || line.size() < 12 || line[8] != ' ')
        {
            this->fail(proxy::Error::InvalidResponse);
            return;
        }

        int status = 0;
        const auto *statusEnd = line.data() + 12;
        auto [ptr, err] = std::from_chars(line.data() + 9, statusEnd, status);
        if (err != std::errc{} || ptr != statusEnd)
        {
            this->fail(proxy::Error::InvalidResponse);
            return;
        }

        if (status < 200 || status >= 300)
        {
            this->fail(proxy::Error::HttpStatus, status);
            return;
        }

        this->finish({});
    }

    // MARK: SOCKS5

    void socksGreet()
    {
        this->out.clear();
        this->push(SOCKS_VERSION);
        if (this->proxy.hasCredentials())
        {
            this->push(2);
            this->push(SOCKS_AUTH_NONE);
            this->push(SOCKS_AUTH_PASSWORD);
        }
        else
        {
            this->push(1);
            this->push(SOCKS_AUTH_NONE);
        }

        this->writeOut(&HandshakeOp::socksReadMethod);
    }

    void socksReadMethod()
    {
        this->readIn(2, &HandshakeOp::onSocksMethod);
    }

    void onSocksMethod()
    {
        if (this->buf[0] != SOCKS_VERSION)
        {
            this->fail(proxy::Error::InvalidResponse);
            return;
        }

        switch (this->buf[1])
        {
            case SOCKS_AUTH_NONE:
                this->socksConnect();
                return;
            case SOCKS_AUTH_PASSWORD:
                if (!this->proxy.hasCredentials())
                {
                    // we didn't offer this
                    this->fail(proxy::Error::InvalidResponse);
                    return;
                }
                this->socksAuth();
                return;
            case SOCKS_AUTH_UNACCEPTABLE:
                this->fail(proxy::Error::NoAcceptableAuthMethod);
                return;
            default:
                this->fail(proxy::Error::InvalidResponse);
                return;
        }
    }

    void socksAuth()
    {
        const auto &user = this->proxy.username;
        const auto &pass = this->proxy.password;
        if (user.size() > 255 || pass.size() > 255)
        {
            this->fail(proxy::Error::CredentialsTooLong);
            return;
        }

        this->out.clear();
        this->push(SOCKS_PASSWORD_VERSION);
        this->push(static_cast<std::uint8_t>(user.size()));
        this->out += user;
        this->push(static_cast<std::uint8_t>(pass.size()));
        this->out += pass;

        this->writeOut(&HandshakeOp::socksReadAuth);
    }

    void socksReadAuth()
    {
        this->readIn(2, &HandshakeOp::onSocksAuth);
    }

    void onSocksAuth()
    {
        if (this->buf[0] != SOCKS_PASSWORD_VERSION)
        {
            this->fail(proxy::Error::InvalidResponse);
            return;
        }
        if (this->buf[1] != 0)
        {
            this->fail(proxy::Error::AuthFailed);
            return;
        }

        this->socksConnect();
    }

    void socksConnect()
    {
        this->out.clear();
        this->push(SOCKS_VERSION);
        this->push(SOCKS_CMD_CONNECT);
        this->push(0);  // reserved

        error_code ec;
        auto addr = asio::ip::make_address(this->host, ec);
        if (!ec && addr.is_v4())
        {
            this->push(SOCKS_ATYP_IPV4);
            for (auto b : addr.to_v4().to_bytes())
            {
                this->push(b);
            }
        }
        else if (!ec && addr.is_v6())
        {
            this->push(SOCKS_ATYP_IPV6);
            for (auto b : addr.to_v6().to_bytes())
            {
                this->push(b);
            }
        }
        else
        {
            // let the proxy resolve the name
            if (this->host.empty() || this->host.size() > 255)
            {
                this->fail(proxy::Error::HostTooLong);
                return;
            }
            this->push(SOCKS_ATYP_DOMAIN);
            this->push(static_cast<std::uint8_t>(this->host.size()));
            this->out += this->host;
        }
        this->push(static_cast<std::uint8_t>(this->port >> 8));
        this->push(static_cast<std::uint8_t>(this->port & 0xff));

        this->writeOut(&HandshakeOp::socksReadReply);
    }

    void socksReadReply()
    {
        // VER REP RSV ATYP, followed by the bound address
        this->readIn(4, &HandshakeOp::onSocksReply);
    }

    void onSocksReply()
    {
        if (this->buf[0] != SOCKS_VERSION)
        {
            this->fail(proxy::Error::InvalidResponse);
            return;
        }
        if (this->buf[1] != 0)
        {
            this->fail(proxy::Error::Socks5Reply, this->buf[1]);
            return;
        }

        // we don't care about the bound address, but it has to be consumed
        switch (this->buf[3])
        {
            case SOCKS_ATYP_IPV4:
                this->readIn(4 + 2, &HandshakeOp::onSocksDone);
                return;
            case SOCKS_ATYP_IPV6:
                this->readIn(16 + 2, &HandshakeOp::onSocksDone);
                return;
            case SOCKS_ATYP_DOMAIN:
                this->readIn(1, &HandshakeOp::onSocksBoundNameLength);
                return;
            default:
                this->fail(proxy::Error::InvalidResponse);
                return;
        }
    }

    void onSocksBoundNameLength()
    {
        this->readIn(std::size_t{this->buf[0]} + 2, &HandshakeOp::onSocksDone);
    }

    void onSocksDone()
    {
        this->finish({});
    }

    void push(std::uint8_t byte)
    {
        this->out.push_back(static_cast<char>(byte));
    }

    beast::tcp_stream &stream;
    ProxyConfig proxy;
    std::string host;
    std::uint16_t port;
    ProxyHandshakeHandler handler;

    std::string out;
    std::string response;
    // large enough for a SOCKS5 domain name (1 + 255) and a port
    std::array<std::uint8_t, 258> buf{};
};

}  // namespace

void asyncProxyHandshake(beast::tcp_stream &stream, const ProxyConfig &proxy,
                         std::string targetHost, std::uint16_t targetPort,
                         ProxyHandshakeHandler handler)
{
    std::make_shared<HandshakeOp>(stream, proxy, std::move(targetHost),
                                  targetPort, std::move(handler))
        ->start();
}

}  // namespace chatterino::eventsub::lib

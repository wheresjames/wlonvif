#pragma once

/**
 * @file web_server.hpp
 * @brief Small synchronous HTTP/WebSocket server shared by the browser examples.
 *
 * The server owns the transport policy every browser example needs: loopback-only
 * serving unless TLS and --allow-remote are both given, bounded header and body
 * sizes, per-phase socket deadlines, a connection limit, and security headers.
 * Applications only decide what each request means.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <wlonvif/wlonvif.hpp>

namespace wlonvif::examples::web
{
    /// One parsed browser request.
    struct Request
    {
        std::string method;       ///< Upper-case method, such as `GET`.
        std::string target;       ///< Path and query, such as `/api/state?x=1`.
        std::string body;         ///< Request body, bounded by ServerOptions::body_limit.
        std::string host;         ///< Host header.
        std::string origin;       ///< Origin header; empty for same-origin navigation.
        std::string cookie;       ///< Raw Cookie header.
        std::string content_type; ///< Content-Type header.
        bool secure{};            ///< Whether the connection uses TLS.
        bool websocket{};         ///< Whether this is a WebSocket upgrade.

        /// Path without the query string.
        std::string_view path() const;
        /// Value of query parameter @p name, percent-decoded; empty when absent.
        std::string query(std::string_view name) const;
        /// Value of cookie @p name; empty when absent.
        std::string cookie_value(std::string_view name) const;
    };

    /// A complete response.
    struct Response
    {
        unsigned status{200};                                     ///< HTTP status code.
        std::string content_type{"application/json"};             ///< Content-Type.
        std::string body;                                         ///< Response body.
        std::vector<std::pair<std::string, std::string>> headers; ///< Extra headers.
    };

    /// A response written incrementally, such as a multipart MJPEG stream.
    class ResponseStream
    {
      public:
        virtual ~ResponseStream() = default;
        /// Send the status line and headers; must precede write().
        virtual bool begin(
            unsigned status,
            std::string_view content_type,
            const std::vector<std::pair<std::string, std::string>>& headers = {}
        ) = 0;
        /// Send body bytes; false once the client is gone or the write stalled.
        virtual bool write(std::string_view bytes) = 0;
        /// Whether the server is shutting down.
        virtual bool stopping() const = 0;
        /**
         * @brief Cap the connection's kernel send buffer at about @p bytes.
         *
         * For live media: a large buffer lets a slow client fall seconds behind
         * on queued data, while a small one makes write() wait, so the sender
         * skips stale content instead.
         */
        virtual void limit_send_buffer(std::size_t bytes)
        {
            (void)bytes;
        }
    };

    /// A WebSocket connection carrying text messages.
    class MessageChannel
    {
      public:
        virtual ~MessageChannel() = default;
        /// Wait up to @p timeout for the next message; false when the peer is gone.
        virtual bool read(std::string& message, std::chrono::milliseconds timeout) = 0;
        /// Send a text message; false when the peer is gone or the write stalled.
        virtual bool write(std::string_view message) = 0;
    };

    /// What an application serves.
    class Application
    {
      public:
        virtual ~Application() = default;
        /// Answer an ordinary request.
        virtual Response handle(const Request& request) = 0;
        /// Return true to answer @p request with stream() instead of handle().
        virtual bool streams(const Request&)
        {
            return false;
        }
        /// Write a streaming response.
        virtual void stream(const Request&, ResponseStream&)
        {
        }
        /// Return true to accept a WebSocket upgrade for @p request.
        virtual bool accepts(const Request&)
        {
            return false;
        }
        /// Serve an accepted WebSocket connection until it ends.
        virtual void serve(const Request&, MessageChannel&)
        {
        }
        /// Content-Security-Policy sent with every complete response.
        virtual std::string content_security_policy() const
        {
            return "default-src 'self'; connect-src 'self' ws: wss:; img-src 'self' data: blob:; "
                   "media-src 'self' http: https: blob:; style-src 'self' 'unsafe-inline'; "
                   "frame-ancestors 'none'";
        }
        /// Called once the server stops accepting connections.
        virtual void stopped()
        {
        }
    };

    /// Listening and resource policy.
    struct ServerOptions
    {
        std::string address{"127.0.0.1"};    ///< Listen address.
        std::uint16_t port{8080};            ///< Listen port.
        bool allow_remote{};                 ///< Permit non-loopback serving (requires TLS).
        std::string certificate_file;        ///< PEM certificate chain enabling TLS.
        std::string private_key_file;        ///< PEM private key.
        std::size_t maximum_connections{16}; ///< Simultaneous connections.
        std::size_t body_limit{4096};        ///< Largest accepted request body.
        std::size_t message_limit{4096};     ///< Largest accepted WebSocket message.
        /// Bound on TLS negotiation, the request header, and the WebSocket upgrade.
        std::chrono::milliseconds handshake_timeout{5000};
        /// Bound on any single write to a stalled client.
        std::chrono::milliseconds write_timeout{10000};
        std::string server_name{"wlonvif"}; ///< Server header value.
    };

    /// Encode @p text as a JSON string literal, quotes included.
    std::string json_string(std::string_view text);

    /// Return 64 hexadecimal characters from the system CSPRNG, or empty on failure.
    std::string random_token();

    /// Accept connections and dispatch them to an Application until stopped.
    class Server
    {
      public:
        Server(ServerOptions options, std::shared_ptr<Application> application);
        /// Serve until @p stop_requested becomes true; fails fast on bad options.
        Result<void> run(std::atomic_bool& stop_requested);

      private:
        struct Impl;
        std::shared_ptr<Impl> impl_;
    };
} // namespace wlonvif::examples::web

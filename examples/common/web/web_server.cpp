/**
 * @file web_server.cpp
 * @brief A small synchronous HTTP, WebSocket, and TLS server on Boost.Beast.
 *
 * One thread per connection, which keeps the request handlers plain blocking
 * code. The accept loop doubles as a watchdog: every socket phase (handshake,
 * request read, response write) has a deadline, and the loop shuts down sockets
 * whose deadline passed, which unblocks their thread. Every response is sent
 * with `Connection: close`, so a connection never sits idle between requests.
 *
 * Safety defaults: loopback only unless TLS and allow_remote are both set; a
 * cap on connections, header size, and body size; and security headers
 * (CSP, no framing, no sniffing, no referrer) on every response.
 */

#include "web_server.hpp"

#include <array>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <mutex>
#include <openssl/rand.h>
#include <optional>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace wlonvif::examples::web
{
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace websocket = beast::websocket;
    using tcp = asio::ip::tcp;

    std::string_view Request::path() const
    {
        const std::string_view value(target);
        return value.substr(0, value.find('?'));
    }

    std::string Request::query(std::string_view name) const
    {
        const auto mark = target.find('?');
        if (mark == std::string::npos)
        {
            return {};
        }
        std::string_view rest = std::string_view(target).substr(mark + 1);
        while (!rest.empty())
        {
            const auto end = rest.find('&');
            const auto pair = rest.substr(0, end);
            const auto equals = pair.find('=');
            if (pair.substr(0, equals) == name)
            {
                std::string decoded;
                const auto value =
                    equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1);
                for (std::size_t index = 0; index < value.size(); ++index)
                {
                    if (value[index] == '+')
                    {
                        decoded += ' ';
                    }
                    else if (value[index] == '%' && index + 2 < value.size() &&
                             std::isxdigit(static_cast<unsigned char>(value[index + 1])) &&
                             std::isxdigit(static_cast<unsigned char>(value[index + 2])))
                    {
                        decoded += static_cast<char>(
                            std::stoi(std::string(value.substr(index + 1, 2)), nullptr, 16)
                        );
                        index += 2;
                    }
                    else
                    {
                        decoded += value[index];
                    }
                }
                return decoded;
            }
            if (end == std::string_view::npos)
            {
                break;
            }
            rest.remove_prefix(end + 1);
        }
        return {};
    }

    std::string Request::cookie_value(std::string_view name) const
    {
        std::string_view rest(cookie);
        while (!rest.empty())
        {
            const auto end = rest.find(';');
            auto pair = rest.substr(0, end);
            while (!pair.empty() && pair.front() == ' ')
            {
                pair.remove_prefix(1);
            }
            const auto equals = pair.find('=');
            if (equals != std::string_view::npos && pair.substr(0, equals) == name)
            {
                return std::string(pair.substr(equals + 1));
            }
            if (end == std::string_view::npos)
            {
                break;
            }
            rest.remove_prefix(end + 1);
        }
        return {};
    }

    std::string json_string(std::string_view text)
    {
        std::string output = "\"";
        for (const char value : text)
        {
            const auto byte = static_cast<unsigned char>(value);
            switch (value)
            {
                case '"':
                    output += "\\\"";
                    break;
                case '\\':
                    output += "\\\\";
                    break;
                case '\n':
                    output += "\\n";
                    break;
                case '\r':
                    output += "\\r";
                    break;
                case '\t':
                    output += "\\t";
                    break;
                default:
                    if (byte < 0x20)
                    {
                        constexpr char hex[] = "0123456789abcdef";
                        output += "\\u00";
                        output += hex[byte >> 4];
                        output += hex[byte & 15];
                    }
                    else
                    {
                        output += value;
                    }
            }
        }
        return output + '"';
    }

    std::string random_token()
    {
        std::array<unsigned char, 32> bytes{};
        if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        {
            return {};
        }
        constexpr char hex[] = "0123456789abcdef";
        std::string result(bytes.size() * 2, '0');
        for (std::size_t i = 0; i < bytes.size(); ++i)
        {
            result[i * 2] = hex[bytes[i] >> 4];
            result[i * 2 + 1] = hex[bytes[i] & 15];
        }
        return result;
    }

    namespace
    {
        Error server_error(std::string message)
        {
            return Error{ErrorCategory::network, OperationId::custom, std::move(message)};
        }
    } // namespace

    struct Server::Impl
    {
        /// Interruptible socket handle plus the deadline of its current phase.
        struct Connection
        {
            using NativeHandle = tcp::socket::native_handle_type;
            std::mutex mutex;
#ifdef _WIN32
            NativeHandle handle{INVALID_SOCKET};
#else
            NativeHandle handle{-1};
#endif
            std::chrono::steady_clock::time_point deadline{
                std::chrono::steady_clock::time_point::max()
            };

            void bind(tcp::socket& socket)
            {
                std::lock_guard lock(mutex);
                handle = socket.native_handle();
            }
            void clear()
            {
                std::lock_guard lock(mutex);
#ifdef _WIN32
                handle = INVALID_SOCKET;
#else
                handle = -1;
#endif
            }
            /**
             * @brief Require the current socket phase to finish within @p timeout.
             *
             * Beast's tcp_stream expiry and WebSocket timeout options apply only
             * to asynchronous operations; this server is synchronous, so deadlines
             * are enforced by the accept loop, which interrupts expired sockets.
             */
            void arm(std::chrono::steady_clock::duration timeout)
            {
                std::lock_guard lock(mutex);
                deadline = std::chrono::steady_clock::now() + timeout;
            }
            /// Suspend the deadline while no socket operation is pending.
            void disarm()
            {
                std::lock_guard lock(mutex);
                deadline = std::chrono::steady_clock::time_point::max();
            }
            bool expired(std::chrono::steady_clock::time_point now)
            {
                std::lock_guard lock(mutex);
                return now >= deadline;
            }
            void interrupt() noexcept
            {
                std::lock_guard lock(mutex);
#ifdef _WIN32
                if (handle != INVALID_SOCKET)
                {
                    ::shutdown(handle, SD_BOTH);
                }
#else
                if (handle >= 0)
                {
                    ::shutdown(handle, SHUT_RDWR);
                }
#endif
            }
        };

        ServerOptions options;
        std::shared_ptr<Application> application;
        std::atomic_size_t active_connections{};
        std::atomic_bool shutting_down{};
        asio::io_context io;
        asio::ssl::context tls{asio::ssl::context::tls_server};

        Impl(ServerOptions value, std::shared_ptr<Application> app)
            : options(std::move(value)), application(std::move(app))
        {
        }

        template <class Stream> class StreamWriter final : public ResponseStream
        {
          public:
            StreamWriter(Stream& stream, Impl& owner, Connection& connection, unsigned version)
                : stream_(stream), owner_(owner), connection_(connection), version_(version)
            {
            }
            bool begin(
                unsigned status,
                std::string_view content_type,
                const std::vector<std::pair<std::string, std::string>>& headers
            ) override
            {
                http::response<http::empty_body> header{
                    static_cast<http::status>(status), version_
                };
                for (const auto& [name, value] : headers)
                {
                    header.set(name, value);
                }
                header.set(http::field::server, owner_.options.server_name);
                header.set(http::field::content_type, content_type);
                header.set(http::field::cache_control, "no-store");
                header.set("X-Content-Type-Options", "nosniff");
                header.keep_alive(false);
                http::response_serializer<http::empty_body> serializer{header};
                connection_.arm(owner_.options.write_timeout);
                http::write_header(stream_, serializer, error_);
                connection_.disarm();
                return !error_;
            }
            bool write(std::string_view bytes) override
            {
                if (error_)
                {
                    return false;
                }
                // Re-armed per write: a reader that stops consuming stalls the
                // write, and the watchdog then releases this worker.
                connection_.arm(owner_.options.write_timeout);
                asio::write(stream_, asio::buffer(bytes.data(), bytes.size()), error_);
                connection_.disarm();
                return !error_;
            }
            bool stopping() const override
            {
                return owner_.shutting_down.load();
            }
            void limit_send_buffer(std::size_t bytes) override
            {
                beast::error_code ignored;
                beast::get_lowest_layer(stream_).socket().set_option(
                    asio::socket_base::send_buffer_size(static_cast<int>(bytes)), ignored
                );
            }

          private:
            Stream& stream_;
            Impl& owner_;
            Connection& connection_;
            unsigned version_;
            beast::error_code error_;
        };

        template <class Stream> class SocketChannel final : public MessageChannel
        {
          public:
            SocketChannel(websocket::stream<Stream>& socket, Impl& owner, Connection& connection)
                : socket_(socket), owner_(owner), connection_(connection)
            {
            }
            bool read(std::string& message, std::chrono::milliseconds timeout) override
            {
                beast::flat_buffer buffer;
                beast::error_code error;
                connection_.arm(timeout);
                socket_.read(buffer, error);
                connection_.disarm();
                if (error)
                {
                    return false;
                }
                message = beast::buffers_to_string(buffer.data());
                return true;
            }
            bool write(std::string_view message) override
            {
                beast::error_code error;
                socket_.text(true);
                connection_.arm(owner_.options.write_timeout);
                socket_.write(asio::buffer(message.data(), message.size()), error);
                connection_.disarm();
                return !error;
            }

          private:
            websocket::stream<Stream>& socket_;
            Impl& owner_;
            Connection& connection_;
        };

        /// Read one request and answer it: a page or API call, a stream, or a WebSocket.
        template <class Stream>
        void serve(Stream stream, bool secure, const std::shared_ptr<Connection>& connection)
        {
            connection->bind(beast::get_lowest_layer(stream).socket());
            connection->arm(options.handshake_timeout);
            beast::flat_buffer buffer;
            http::request_parser<http::string_body> parser;
            parser.header_limit(8192);
            parser.body_limit(options.body_limit);
            beast::error_code error;
            http::read(stream, buffer, parser, error);
            if (error == http::error::body_limit)
            {
                http::response<http::string_body> response{http::status::payload_too_large, 11};
                response.set(http::field::content_type, "application/json");
                response.body() =
                    R"({"ok":false,"error":{"category":"invalid-argument","message":"request body too large"}})";
                response.keep_alive(false);
                response.prepare_payload();
                connection->arm(options.write_timeout);
                http::write(stream, response, error);
                return;
            }
            if (error)
            {
                return;
            }
            // The handshake deadline covers reading the request only; the
            // application may take as long as its work needs to answer.
            connection->disarm();
            auto message = parser.release();
            Request request;
            request.method = std::string(message.method_string());
            request.target = std::string(message.target());
            request.body = std::move(message.body());
            request.host = std::string(message[http::field::host]);
            request.origin = std::string(message[http::field::origin]);
            request.cookie = std::string(message[http::field::cookie]);
            request.content_type = std::string(message[http::field::content_type]);
            request.secure = secure;
            request.websocket = websocket::is_upgrade(message);

            if (request.websocket)
            {
                if (!application->accepts(request))
                {
                    return;
                }
                websocket::stream<Stream> socket(std::move(stream));
                connection->bind(beast::get_lowest_layer(socket).socket());
                socket.read_message_max(options.message_limit);
                socket.accept(message, error);
                connection->disarm();
                if (error)
                {
                    return;
                }
                SocketChannel<Stream> channel(socket, *this, *connection);
                application->serve(request, channel);
                return;
            }
            if (application->streams(request))
            {
                connection->disarm();
                StreamWriter<Stream> writer(stream, *this, *connection, message.version());
                application->stream(request, writer);
                return;
            }

            const auto answer = application->handle(request);
            http::response<http::string_body> response{
                static_cast<http::status>(answer.status), message.version()
            };
            response.set(http::field::server, options.server_name);
            response.set(http::field::content_type, answer.content_type);
            response.set(http::field::cache_control, "no-store");
            response.set("X-Content-Type-Options", "nosniff");
            response.set("X-Frame-Options", "DENY");
            response.set("Referrer-Policy", "no-referrer");
            response.set("Content-Security-Policy", application->content_security_policy());
            for (const auto& [name, value] : answer.headers)
            {
                response.set(name, value);
            }
            response.keep_alive(false);
            response.body() = answer.body;
            response.prepare_payload();
            connection->arm(options.write_timeout);
            http::write(stream, response, error);
        }
    };

    Server::Server(ServerOptions options, std::shared_ptr<Application> application)
        : impl_(std::make_shared<Impl>(std::move(options), std::move(application)))
    {
    }

    Result<void> Server::run(std::atomic_bool& stop_requested)
    {
        beast::error_code error;
        const auto address = asio::ip::make_address(impl_->options.address, error);
        if (error)
        {
            return server_error("invalid listen address");
        }
        const bool secure = !impl_->options.certificate_file.empty();
        if (!address.is_loopback() && (!impl_->options.allow_remote || !secure))
        {
            return server_error("non-loopback serving requires --allow-remote and TLS");
        }
        if (secure != !impl_->options.private_key_file.empty())
        {
            return server_error("TLS requires both certificate and private key files");
        }
        if (secure)
        {
            impl_->tls.set_options(
                asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
                asio::ssl::context::no_tlsv1_1
            );
            impl_->tls.use_certificate_chain_file(impl_->options.certificate_file, error);
            if (error)
            {
                return server_error("unable to load TLS certificate");
            }
            impl_->tls.use_private_key_file(
                impl_->options.private_key_file, asio::ssl::context::pem, error
            );
            if (error)
            {
                return server_error("unable to load TLS private key");
            }
        }
        tcp::acceptor acceptor(impl_->io);
        acceptor.open(address.is_v6() ? tcp::v6() : tcp::v4(), error);
        if (error)
        {
            return server_error(error.message());
        }
        acceptor.set_option(asio::socket_base::reuse_address(true), error);
        acceptor.bind({address, impl_->options.port}, error);
        if (error)
        {
            return server_error(error.message());
        }
        acceptor.listen(static_cast<int>(impl_->options.maximum_connections), error);
        if (error)
        {
            return server_error(error.message());
        }
        acceptor.non_blocking(true, error);
        struct Worker
        {
            std::thread thread;
            std::shared_ptr<std::atomic_bool> done;
            std::shared_ptr<Impl::Connection> connection;
        };
        std::vector<Worker> workers;
        // The acceptor is non-blocking, so this loop wakes every 20 ms to enforce
        // deadlines and reap finished workers even when no client connects.
        while (!stop_requested.load())
        {
            const auto now = std::chrono::steady_clock::now();
            for (auto it = workers.begin(); it != workers.end();)
            {
                if (!it->done->load() && it->connection->expired(now))
                {
                    it->connection->interrupt();
                }
                if (it->done->load())
                {
                    if (it->thread.joinable())
                    {
                        it->thread.join();
                    }
                    it = workers.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            tcp::socket socket(impl_->io);
            acceptor.accept(socket, error);
            if (error == asio::error::would_block || error == asio::error::try_again)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (error)
            {
                break;
            }
            if (impl_->active_connections.load() >= impl_->options.maximum_connections)
            {
                socket.close();
                continue;
            }
            ++impl_->active_connections;
            auto impl = impl_;
            auto done = std::make_shared<std::atomic_bool>();
            auto connection = std::make_shared<Impl::Connection>();
            // Publish an interruptible native handle before the worker starts. A
            // signal can otherwise arrive between thread creation and its first
            // instruction, leaving shutdown with no descriptor to wake.
            connection->bind(socket);
            workers.push_back(
                {std::thread(
                     [impl, socket = std::move(socket), secure, done, connection]() mutable
                     {
                         if (secure)
                         {
                             beast::ssl_stream<beast::tcp_stream> stream(
                                 std::move(socket), impl->tls
                             );
                             connection->bind(beast::get_lowest_layer(stream).socket());
                             connection->arm(impl->options.handshake_timeout);
                             beast::error_code handshake;
                             stream.handshake(asio::ssl::stream_base::server, handshake);
                             if (!handshake)
                             {
                                 impl->serve(std::move(stream), true, connection);
                             }
                         }
                         else
                         {
                             impl->serve(beast::tcp_stream(std::move(socket)), false, connection);
                         }
                         connection->clear();
                         --impl->active_connections;
                         done->store(true);
                     }
                 ),
                 done,
                 connection}
            );
        }
        // Shutting down: wake every worker blocked on its socket, then wait for them.
        acceptor.close();
        impl_->shutting_down.store(true);
        for (auto& worker : workers)
        {
            worker.connection->interrupt();
        }
        for (auto& worker : workers)
        {
            if (worker.thread.joinable())
            {
                worker.thread.join();
            }
        }
        impl_->application->stopped();
        return error && error != asio::error::operation_aborted &&
                       error != asio::error::would_block && error != asio::error::try_again
                   ? Result<void>{server_error(error.message())}
                   : Result<void>{};
    }
} // namespace wlonvif::examples::web

#include "server.hpp"
#include "assets.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <openssl/rand.h>

#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace wlonvif::examples::ptz_web
{
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace websocket = beast::websocket;
    using tcp = asio::ip::tcp;

    namespace
    {
        std::string random_token()
        {
            std::array<unsigned char, 32> bytes{};
            if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) return {};
            constexpr char hex[] = "0123456789abcdef";
            std::string result(bytes.size() * 2, '0');
            for (std::size_t i = 0; i < bytes.size(); ++i)
            {
                result[i * 2] = hex[bytes[i] >> 4];
                result[i * 2 + 1] = hex[bytes[i] & 15];
            }
            return result;
        }
        std::string cookie_value(std::string_view cookie)
        {
            constexpr std::string_view name = "wlonvif_ptz=";
            const auto start = cookie.find(name);
            if (start == std::string_view::npos) return {};
            const auto value = cookie.substr(start + name.size());
            const auto end = value.find(';');
            return std::string(value.substr(0, end));
        }
        bool loopback(const asio::ip::address& address) { return address.is_loopback(); }
        Error server_error(std::string message)
        {
            return Error{ErrorCategory::network, OperationId::continuous_move, std::move(message)};
        }
        template<class Body, class Allocator>
        void security_headers(http::response<Body>& response,
                              const http::request<http::basic_string_body<char>,
                              http::basic_fields<Allocator>>&)
        {
            response.set(http::field::cache_control, "no-store");
            response.set("X-Content-Type-Options", "nosniff");
            response.set("X-Frame-Options", "DENY");
            response.set("Referrer-Policy", "no-referrer");
            response.set("Content-Security-Policy",
                "default-src 'self'; connect-src 'self' ws: wss:; script-src 'nonce-wlonvif-static-ui'; "
                "style-src 'unsafe-inline'; img-src 'self' data:; media-src http: https: blob:; "
                "frame-ancestors 'none'");
        }
    }

    struct WebServer::Impl
    {
        struct Connection
        {
            using NativeHandle = tcp::socket::native_handle_type;
            std::mutex mutex;
#ifdef _WIN32
            NativeHandle handle{INVALID_SOCKET};
#else
            NativeHandle handle{-1};
#endif

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
            void interrupt() noexcept
            {
                std::lock_guard lock(mutex);
#ifdef _WIN32
                if (handle != INVALID_SOCKET) ::shutdown(handle, SD_BOTH);
#else
                if (handle >= 0) ::shutdown(handle, SHUT_RDWR);
#endif
            }
        };

        WebServerConfig config;
        ControlProtocol protocol;
        CommandHandler command;
        DisconnectHandler disconnect;
        std::mutex protocol_mutex;
        std::atomic_size_t active_connections{};
        std::atomic_bool shutting_down{};
        asio::io_context io;
        asio::ssl::context tls{asio::ssl::context::tls_server};

        Impl(WebServerConfig cfg, CommandHandler cmd, DisconnectHandler gone)
            : config(std::move(cfg)), protocol(config.protocol), command(std::move(cmd)),
              disconnect(std::move(gone)) {}

        template<class Stream>
        void serve(Stream stream, bool secure, const std::shared_ptr<Connection>& connection)
        {
            connection->bind(beast::get_lowest_layer(stream).socket());
            beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
            beast::flat_buffer buffer;
            http::request_parser<http::string_body> parser;
            parser.header_limit(8192);
            parser.body_limit(4096);
            beast::error_code error;
            http::read(stream, buffer, parser, error);
            if (error) return;
            auto request = parser.release();
            const std::string origin = request[http::field::origin];
            if (websocket::is_upgrade(request) && request.target() == "/control")
            {
                const std::string session = cookie_value(request[http::field::cookie]);
                {
                    std::lock_guard lock(protocol_mutex);
                    if (!protocol.origin_allowed(origin) || session.empty()) return;
                }
                websocket::stream<Stream> ws(std::move(stream));
                connection->bind(beast::get_lowest_layer(ws).socket());
                websocket::stream_base::timeout timeout{
                    config.protocol.lease, config.protocol.lease, false};
                ws.set_option(timeout);
                ws.read_message_max(config.protocol.maximum_message_bytes);
                ws.accept(request, error);
                if (error) return;
                while (true)
                {
                    beast::flat_buffer message;
                    ws.read(message, error);
                    if (error) break;
                    const auto payload = beast::buffers_to_string(message.data());
                    Result<WebCommand> parsed = Error{};
                    {
                        std::lock_guard lock(protocol_mutex);
                        parsed = protocol.handle(session, origin, payload,
                            std::chrono::steady_clock::now());
                    }
                    std::string reply;
                    if (!parsed) reply = "{\"ok\":false,\"error\":\"" + parsed.error().message + "\"}";
                    else
                    {
                        auto result = command(parsed.value());
                        reply = result ? "{\"ok\":true,\"telemetry\":" + result.value() + "}"
                                       : "{\"ok\":false,\"error\":\"" + result.error().message + "\"}";
                    }
                    ws.text(true);
                    ws.write(asio::buffer(reply), error);
                    if (error) break;
                }
                std::optional<std::string> removed;
                {
                    std::lock_guard lock(protocol_mutex);
                    removed = protocol.disconnect(session);
                }
                if (removed) disconnect(*removed);
                return;
            }

            if (request.method() == http::verb::get && request.target() == "/video.mjpeg" &&
                config.video)
            {
                const std::string session = cookie_value(request[http::field::cookie]);
                {
                    std::lock_guard lock(protocol_mutex);
                    if (!protocol.session_valid(session, std::chrono::steady_clock::now())) return;
                }
                http::response<http::empty_body> header{http::status::ok, request.version()};
                header.set(http::field::server, "wlonvif-ptz-web");
                header.set(http::field::content_type,
                           "multipart/x-mixed-replace; boundary=wlonvif-frame");
                header.set(http::field::cache_control, "no-store");
                header.keep_alive(false);
                http::response_serializer<http::empty_body> serializer{header};
                http::write_header(stream, serializer, error);
                std::uint64_t sequence{};
                while (!error && !shutting_down.load())
                {
                    std::string jpeg;
                    if (!config.video->wait_for_frame(sequence, jpeg, std::chrono::seconds(1)))
                    {
                        asio::write(stream, asio::buffer(std::string_view{"\r\n"}), error);
                        continue;
                    }
                    const std::string prefix = "--wlonvif-frame\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                        std::to_string(jpeg.size()) + "\r\n\r\n";
                    asio::write(stream, asio::buffer(prefix), error);
                    if (!error) asio::write(stream, asio::buffer(jpeg), error);
                    if (!error) asio::write(stream, asio::buffer(std::string_view{"\r\n"}), error);
                }
                return;
            }

            http::response<http::string_body> response{http::status::not_found, request.version()};
            response.set(http::field::server, "wlonvif-ptz-web");
            response.keep_alive(false);
            if (request.method() == http::verb::get && request.target() == "/")
            {
                response.result(http::status::ok);
                response.set(http::field::content_type, "text/html; charset=utf-8");
                response.body() = web_ui;
                const auto marker = response.body().find("__FFMPEG_ENABLED__");
                if (marker != std::string::npos)
                    response.body().replace(marker, std::string_view{"__FFMPEG_ENABLED__"}.size(),
                                            config.video ? "true" : "false");
                const auto configuration = response.body().find("__PTZ_CONFIGURATION__");
                if (configuration != std::string::npos)
                    response.body().replace(configuration,
                        std::string_view{"__PTZ_CONFIGURATION__"}.size(), config.ui_configuration);
            }
            else if (request.method() == http::verb::get && request.target() == "/health")
            {
                response.result(http::status::ok);
                response.set(http::field::content_type, "application/json");
                response.body() = "{\"status\":\"ok\"}";
            }
            else if (request.method() == http::verb::post && request.target() == "/api/session")
            {
                const auto token = random_token();
                Result<std::string> session = Error{};
                {
                    std::lock_guard lock(protocol_mutex);
                    session = protocol.create_session(
                        origin, std::chrono::steady_clock::now(), token);
                }
                if (session)
                {
                    response.result(http::status::no_content);
                    response.set(http::field::set_cookie, "wlonvif_ptz=" + token +
                        "; Path=/; HttpOnly; SameSite=Strict" + (secure ? "; Secure" : ""));
                    response.body().clear();
                }
                else
                {
                    response.result(http::status::unauthorized);
                    response.body() = "session creation failed";
                }
            }
            security_headers(response, request);
            response.prepare_payload();
            http::write(stream, response, error);
        }
    };

    WebServer::WebServer(WebServerConfig config, CommandHandler command, DisconnectHandler disconnect)
        : impl_(std::make_shared<Impl>(std::move(config), std::move(command), std::move(disconnect))) {}

    Result<void> WebServer::run(std::atomic_bool& stop_requested)
    {
        beast::error_code error;
        const auto address = asio::ip::make_address(impl_->config.address, error);
        if (error) return server_error("invalid listen address");
        const bool secure = !impl_->config.certificate_file.empty();
        if (!loopback(address) && (!impl_->config.allow_remote || !secure))
            return server_error("non-loopback serving requires --allow-remote and TLS");
        if (secure != !impl_->config.private_key_file.empty())
            return server_error("TLS requires both certificate and private key files");
        if (secure)
        {
            impl_->tls.set_options(asio::ssl::context::default_workarounds |
                                   asio::ssl::context::no_sslv2 |
                                   asio::ssl::context::no_sslv3 |
                                   asio::ssl::context::no_tlsv1 |
                                   asio::ssl::context::no_tlsv1_1);
            impl_->tls.use_certificate_chain_file(impl_->config.certificate_file, error);
            if (error) return server_error("unable to load TLS certificate");
            impl_->tls.use_private_key_file(impl_->config.private_key_file,
                                            asio::ssl::context::pem, error);
            if (error) return server_error("unable to load TLS private key");
        }
        tcp::acceptor acceptor(impl_->io);
        acceptor.open(address.is_v6() ? tcp::v6() : tcp::v4(), error);
        if (error) return server_error(error.message());
        acceptor.set_option(asio::socket_base::reuse_address(true), error);
        acceptor.bind({address, impl_->config.port}, error);
        if (error) return server_error(error.message());
        acceptor.listen(static_cast<int>(impl_->config.maximum_connections), error);
        if (error) return server_error(error.message());
        acceptor.non_blocking(true, error);
        struct Worker
        {
            std::thread thread;
            std::shared_ptr<std::atomic_bool> done;
            std::shared_ptr<Impl::Connection> connection;
        };
        std::vector<Worker> workers;
        while (!stop_requested.load())
        {
            for (auto it = workers.begin(); it != workers.end();)
            {
                if (it->done->load())
                {
                    if (it->thread.joinable()) it->thread.join();
                    it = workers.erase(it);
                }
                else ++it;
            }
            tcp::socket socket(impl_->io);
            acceptor.accept(socket, error);
            if (error == asio::error::would_block || error == asio::error::try_again)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (error) break;
            if (impl_->active_connections.load() >= impl_->config.maximum_connections)
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
            workers.push_back({std::thread([impl, socket = std::move(socket), secure, done, connection]() mutable
            {
                if (secure)
                {
                    beast::ssl_stream<beast::tcp_stream> stream(std::move(socket), impl->tls);
                    connection->bind(beast::get_lowest_layer(stream).socket());
                    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
                    beast::error_code handshake;
                    stream.handshake(asio::ssl::stream_base::server, handshake);
                    if (!handshake) impl->serve(std::move(stream), true, connection);
                }
                else impl->serve(beast::tcp_stream(std::move(socket)), false, connection);
                connection->clear();
                --impl->active_connections;
                done->store(true);
            }), done, connection});
        }
        acceptor.close();
        impl_->shutting_down.store(true);
        for (auto& worker : workers) worker.connection->interrupt();
        for (auto& worker : workers) if (worker.thread.joinable()) worker.thread.join();
        {
            std::lock_guard lock(impl_->protocol_mutex);
            impl_->protocol.revoke_all();
        }
        return error && error != asio::error::operation_aborted &&
               error != asio::error::would_block && error != asio::error::try_again
            ? Result<void>{server_error(error.message())} : Result<void>{};
    }
}

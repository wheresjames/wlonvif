/**
 * @file server.cpp
 * @brief The ptz-web routes on top of the shared web server.
 */

#include "server.hpp"

#include "assets.hpp"

#include <mutex>
#include <optional>
#include <web_server.hpp>

namespace wlonvif::examples::ptz_web
{
    namespace
    {
        /// web/index.html, embedded at build time.
        std::string_view page()
        {
            for (const auto& file : assets::files)
            {
                if (file.path == "index.html")
                {
                    return file.body;
                }
            }
            return {};
        }

        std::string error_reply(std::string_view message)
        {
            return "{\"ok\":false,\"error\":" + web::json_string(message) + "}";
        }

        /// The PTZ control page, its session endpoint, MJPEG video, and control socket.
        class PtzApplication final : public web::Application
        {
          public:
            PtzApplication(
                WebServerConfig config, CommandHandler command, DisconnectHandler disconnect
            )
                : config_(std::move(config)), protocol_(config_.protocol),
                  command_(std::move(command)), disconnect_(std::move(disconnect))
            {
            }

            web::Response handle(const web::Request& request) override
            {
                web::Response response;
                response.status = 404;
                response.content_type = "text/plain";
                if (request.method == "GET" && request.target == "/")
                {
                    response.status = 200;
                    response.content_type = "text/html; charset=utf-8";
                    response.body = std::string(page());
                    replace(response.body, "__FFMPEG_ENABLED__", config_.video ? "true" : "false");
                    replace(response.body, "__PTZ_CONFIGURATION__", config_.ui_configuration);
                }
                else if (request.method == "GET" && request.target == "/health")
                {
                    response.status = 200;
                    response.content_type = "application/json";
                    response.body = "{\"status\":\"ok\"}";
                }
                else if (request.method == "POST" && request.target == "/api/session")
                {
                    const auto token = web::random_token();
                    Result<std::string> session = Error{};
                    {
                        std::lock_guard lock(protocol_mutex_);
                        session = protocol_.create_session(
                            request.origin, std::chrono::steady_clock::now(), token
                        );
                    }
                    if (session)
                    {
                        response.status = 204;
                        response.headers.emplace_back(
                            "Set-Cookie",
                            "wlonvif_ptz=" + token + "; Path=/; HttpOnly; SameSite=Strict" +
                                (request.secure ? "; Secure" : "")
                        );
                    }
                    else
                    {
                        response.status = 401;
                        response.body = "session creation failed";
                    }
                }
                return response;
            }

            bool streams(const web::Request& request) override
            {
                return config_.video && request.method == "GET" && request.target == "/video.mjpeg";
            }

            void stream(const web::Request& request, web::ResponseStream& output) override
            {
                {
                    std::lock_guard lock(protocol_mutex_);
                    if (!protocol_.session_valid(
                            request.cookie_value("wlonvif_ptz"), std::chrono::steady_clock::now()
                        ))
                    {
                        return;
                    }
                }
                web::serve_mjpeg(*config_.video, output);
            }

            bool accepts(const web::Request& request) override
            {
                if (request.target != "/control")
                {
                    return false;
                }
                std::lock_guard lock(protocol_mutex_);
                return protocol_.origin_allowed(request.origin) &&
                       !request.cookie_value("wlonvif_ptz").empty();
            }

            void serve(const web::Request& request, web::MessageChannel& channel) override
            {
                const auto session = request.cookie_value("wlonvif_ptz");
                std::string payload;
                // A control client that stays silent past its lease has lost its
                // session anyway; ending the read also frees the worker.
                while (channel.read(payload, config_.protocol.lease))
                {
                    Result<WebCommand> parsed = Error{};
                    {
                        std::lock_guard lock(protocol_mutex_);
                        parsed = protocol_.handle(
                            session, request.origin, payload, std::chrono::steady_clock::now()
                        );
                    }
                    std::string reply;
                    if (!parsed)
                    {
                        reply = error_reply(parsed.error().message);
                    }
                    else
                    {
                        auto result = command_(parsed.value());
                        reply = result ? "{\"ok\":true,\"telemetry\":" + result.value() + "}"
                                       : error_reply(result.error().message);
                    }
                    if (!channel.write(reply))
                    {
                        break;
                    }
                }
                std::optional<std::string> removed;
                {
                    std::lock_guard lock(protocol_mutex_);
                    removed = protocol_.disconnect(session);
                }
                if (removed)
                {
                    disconnect_(*removed);
                }
            }

            std::string content_security_policy() const override
            {
                return "default-src 'self'; connect-src 'self' ws: wss:; script-src "
                       "'nonce-wlonvif-static-ui'; "
                       "style-src 'unsafe-inline'; img-src 'self' data:; media-src http: https: "
                       "blob:; "
                       "frame-ancestors 'none'";
            }

            void stopped() override
            {
                std::lock_guard lock(protocol_mutex_);
                protocol_.revoke_all();
            }

          private:
            static void replace(std::string& text, std::string_view marker, std::string_view value)
            {
                const auto found = text.find(marker);
                if (found != std::string::npos)
                {
                    text.replace(found, marker.size(), value);
                }
            }

            WebServerConfig config_;
            ControlProtocol protocol_;
            CommandHandler command_;
            DisconnectHandler disconnect_;
            std::mutex protocol_mutex_;
        };
    } // namespace

    struct WebServer::Impl
    {
        web::Server server;
    };

    WebServer::WebServer(
        WebServerConfig config, CommandHandler command, DisconnectHandler disconnect
    )
    {
        web::ServerOptions options;
        options.address = config.address;
        options.port = config.port;
        options.allow_remote = config.allow_remote;
        options.certificate_file = config.certificate_file;
        options.private_key_file = config.private_key_file;
        options.maximum_connections = config.maximum_connections;
        options.handshake_timeout = config.handshake_timeout;
        options.write_timeout = config.write_timeout;
        options.message_limit = config.protocol.maximum_message_bytes;
        options.server_name = "wlonvif-ptz-web";
        impl_ = std::make_shared<Impl>(Impl{web::Server(
            std::move(options),
            std::make_shared<PtzApplication>(
                std::move(config), std::move(command), std::move(disconnect)
            )
        )});
    }

    Result<void> WebServer::run(std::atomic_bool& stop_requested)
    {
        return impl_->server.run(stop_requested);
    }
} // namespace wlonvif::examples::ptz_web

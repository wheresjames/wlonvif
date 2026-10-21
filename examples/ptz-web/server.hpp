#pragma once

/**
 * @file server.hpp
 * @brief The ptz-web HTTP server: page, session, MJPEG video, and control WebSocket.
 *
 * A thin layer over the shared web server in common/web: it maps four paths
 * to the control protocol and hands validated commands to a CommandHandler.
 *
 *     GET  /             the page, with the profile list filled in
 *     POST /api/session  start a session; answers with a cookie
 *     GET  /video.mjpeg  live video, when FFmpeg is bridging it
 *     GET  /control      WebSocket for control messages (cookie required)
 */

#include "protocol.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <video_source.hpp>

namespace wlonvif::examples::ptz_web
{
    using web::VideoFrameSource;

    struct WebServerConfig
    {
        std::string address{"127.0.0.1"};
        std::uint16_t port{8080};
        /// Required to listen beyond loopback, and then only with TLS.
        bool allow_remote{};
        std::string certificate_file; ///< PEM chain; enables TLS with private_key_file.
        std::string private_key_file;
        std::size_t maximum_connections{16};
        /// Bound on TLS negotiation, the request header, and the WebSocket upgrade.
        std::chrono::milliseconds handshake_timeout{5000};
        /// Bound on any single response or video-frame write to a stalled client.
        std::chrono::milliseconds write_timeout{10000};
        std::shared_ptr<VideoFrameSource> video; ///< Null when there is no video bridge.
        /// JSON placed into the page: the profiles and what each supports.
        std::string ui_configuration{"{\"profiles\":[],\"selected\":\"\"}"};
        ProtocolConfig protocol;
    };
    /// Carries out a validated command; the reply (telemetry JSON) goes back to the browser.
    using CommandHandler = std::function<Result<std::string>(const WebCommand&)>;
    /// Called with a session's owner name when its socket closes or its lease expires.
    using DisconnectHandler = std::function<void(std::string_view)>;

    class WebServer
    {
      public:
        WebServer(WebServerConfig, CommandHandler, DisconnectHandler);
        /// Serve until @p stop_requested becomes true; fails at once on an unsafe configuration.
        Result<void> run(std::atomic_bool& stop_requested);

      private:
        struct Impl;
        std::shared_ptr<Impl> impl_;
    };
} // namespace wlonvif::examples::ptz_web

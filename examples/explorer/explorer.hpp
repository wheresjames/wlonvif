#pragma once

/**
 * @file explorer.hpp
 * @brief Browser application for discovering cameras and exercising wlonvif on them.
 *
 * The Explorer serves a single-page UI and a JSON API. Every API reply is
 * `{"ok":true,"data":...}` or `{"ok":false,"error":{...}}`, where the error keeps
 * the library's category, operation, HTTP status, and SOAP Fault codes so a user
 * can see exactly what the camera answered.
 */

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <web_server.hpp>
#include <wlonvif/wlonvif.hpp>

namespace wlonvif::examples::explorer
{
    /// Opens a session to a Device service URL; tests substitute a virtual device.
    using Connector = std::function<Result<std::shared_ptr<DeviceSession>>(
        const std::string& device_url, const std::optional<Credentials>& credentials
    )>;

    /// Runs a network discovery; tests substitute a fixed result.
    using Scanner =
        std::function<Result<DiscoveryReport>(NetworkDiscoveryOptions options, CallOptions call)>;

    /// Resolves an address typed by the user to a Device service URL.
    using Resolver = std::function<Result<std::string>(const std::string& address)>;

    /// Explorer behaviour and the integrations it may use.
    struct ExplorerOptions
    {
        int timeout_seconds{10};                        ///< Per-request ONVIF deadline.
        std::optional<Credentials> default_credentials; ///< Used when the UI supplies none.
        std::string ffmpeg;               ///< FFmpeg executable; empty disables video.
        std::filesystem::path recordings; ///< Recording directory; empty disables recording.
        std::size_t maximum_hosts{1024};  ///< Largest automatically scanned network.
        /// Host header values the API accepts, such as `127.0.0.1:8090`; protects
        /// against DNS rebinding, since the API holds camera credentials.
        std::vector<std::string> allowed_hosts;
    };

    class Explorer final : public web::Application, public std::enable_shared_from_this<Explorer>
    {
      public:
        Explorer(
            ExplorerOptions options,
            std::shared_ptr<Runtime> runtime,
            Connector connector,
            Scanner scanner,
            Resolver resolver
        );
        ~Explorer() override;
        web::Response handle(const web::Request& request) override;
        bool streams(const web::Request& request) override;
        void stream(const web::Request& request, web::ResponseStream& output) override;
        std::string content_security_policy() const override;
        void stopped() override;

        /// Session cookie value the UI must present; exposed for tests.
        const std::string& session_token() const
        {
            return token_;
        }

        // Defined in implementation.hpp, which only the explorer's own sources include.
        struct Camera;
        struct Implementation;

      private:
        ExplorerOptions options_;
        std::shared_ptr<Runtime> runtime_;
        Connector connector_;
        Scanner scanner_;
        Resolver resolver_;
        std::string token_;
        std::unique_ptr<Implementation> implementation_;
    };
} // namespace wlonvif::examples::explorer

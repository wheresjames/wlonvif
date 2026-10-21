/**
 * @file main.cpp
 * @brief wlonvif-ptz-web: control a PTZ camera from a browser.
 *
 * Connects to the camera, finds what each media profile can do, and serves a
 * page with a movement pad, keyboard control, click-to-centre, box zoom,
 * presets, and tours. Live video can be bridged through FFmpeg as MJPEG.
 *
 *     wlonvif-ptz-web --ip 192.0.2.10 --username admin --confirm-movement
 *
 * Moving a camera is a physical action, so --confirm-movement is required.
 * --diagnose-input serves the page without a camera, to try the controls.
 * The server listens on loopback only unless TLS and --allow-remote are given.
 */

#include "calibration.hpp"
#include "camera_control.hpp"
#include "example_support.hpp"
#include "ffmpeg_video.hpp"
#include "server.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

using namespace wlonvif;
using namespace wlonvif::examples::ptz_web;

namespace
{
    std::atomic_bool stop_requested{}; ///< Set by Ctrl-C; the server loop and tours watch it.
    void signal_handler(int)
    {
        stop_requested.store(true);
    }
    void usage(const char* program)
    {
        std::cout << "Usage: " << program << " [common options] [web options]\n"
                  << "  --diagnose-input           Serve UI without a camera\n"
                  << "  --url URL | --ip ADDRESS   Camera endpoint\n"
                  << "  --profile TOKEN            Initial profile (default: first advertised)\n"
                  << "  --confirm-movement         Required for camera control\n"
                  << "  --listen ADDRESS           Listen address (default 127.0.0.1)\n"
                  << "  --port PORT                Listen port (default 8080)\n"
                  << "  --origin ORIGIN            Add exact allowed browser Origin\n"
                  << "  --allow-remote             Explicitly permit non-loopback TLS serving\n"
                  << "  --tls-cert FILE            PEM certificate chain\n"
                  << "  --tls-key FILE             PEM private key\n"
                  << "  --viewport-calibration FILE  Versioned viewport calibration JSON\n"
                  << "  --ffmpeg-stream [URL]       Discover or override video converted to MJPEG\n"
                  << "  --ffmpeg PATH               FFmpeg executable (default: ffmpeg)\n";
    }
    /// The lower-case host of a URL, for checking a stream is on the same camera.
    std::string url_host(std::string_view value)
    {
        const auto scheme = value.find("://");
        if (scheme == value.npos)
        {
            return {};
        }
        const auto start = scheme + 3;
        const auto end = value.find_first_of("/?#", start);
        auto authority =
            value.substr(start, end == value.npos ? value.size() - start : end - start);
        if (const auto at = authority.rfind('@'); at != authority.npos)
        {
            authority.remove_prefix(at + 1);
        }
        std::string host;
        if (authority.starts_with('['))
        {
            const auto close = authority.find(']');
            if (close == authority.npos)
            {
                return {};
            }
            host = authority.substr(1, close - 1);
        }
        else
        {
            host = authority.substr(0, authority.find(':'));
        }
        std::ranges::transform(
            host,
            host.begin(),
            [](unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            }
        );
        return host;
    }
    /// @p value escaped for a JSON string inside an inline <script>: no `<`, `>`, or `&`,
    /// so a camera-supplied profile name cannot end the script element.
    std::string script_json_string(std::string_view value)
    {
        auto escaped = wlonvif::examples::json_escape(value);
        std::string result;
        for (const char character : escaped)
        {
            if (character == '<')
            {
                result += "\\u003c";
            }
            else if (character == '>')
            {
                result += "\\u003e";
            }
            else if (character == '&')
            {
                result += "\\u0026";
            }
            else
            {
                result += character;
            }
        }
        return result;
    }
} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::string_view(argv[i]) == "--help")
        {
            usage(argv[0]);
            return 0;
        }
    }
    auto parsed = wlonvif::examples::parse_common_arguments(argc, argv);
    if (!parsed)
    {
        std::cerr << parsed.error().message << '\n';
        return 2;
    }
    auto common = parsed.value().common;
    WebServerConfig server;
    std::string url, address, profile, calibration_file, ffmpeg_stream;
    std::string ffmpeg_executable{"ffmpeg"};
    bool diagnostic = false, confirmed = false, ffmpeg_requested = false;
    for (std::size_t i = 0; i < parsed.value().remaining.size(); ++i)
    {
        const auto& argument = parsed.value().remaining[i];
        const bool takes = argument == "--url" || argument == "--ip" || argument == "--profile" ||
                           argument == "--listen" || argument == "--port" ||
                           argument == "--origin" || argument == "--tls-cert" ||
                           argument == "--tls-key" || argument == "--viewport-calibration" ||
                           argument == "--ffmpeg";
        if (takes && i + 1 >= parsed.value().remaining.size())
        {
            std::cerr << "Missing value for " << argument << '\n';
            return 2;
        }
        if (argument == "--url")
        {
            url = parsed.value().remaining[++i];
        }
        else if (argument == "--ip")
        {
            address = parsed.value().remaining[++i];
        }
        else if (argument == "--profile")
        {
            profile = parsed.value().remaining[++i];
        }
        else if (argument == "--listen")
        {
            server.address = parsed.value().remaining[++i];
        }
        else if (argument == "--port")
        {
            std::size_t port = 0;
            if (!wlonvif::examples::parse_size(parsed.value().remaining[++i], port) || port == 0 ||
                port > 65535)
            {
                std::cerr << "Invalid port\n";
                return 2;
            }
            server.port = static_cast<std::uint16_t>(port);
        }
        else if (argument == "--origin")
        {
            server.protocol.allowed_origins.push_back(parsed.value().remaining[++i]);
        }
        else if (argument == "--tls-cert")
        {
            server.certificate_file = parsed.value().remaining[++i];
        }
        else if (argument == "--tls-key")
        {
            server.private_key_file = parsed.value().remaining[++i];
        }
        else if (argument == "--viewport-calibration")
        {
            calibration_file = parsed.value().remaining[++i];
        }
        else if (argument == "--ffmpeg-stream")
        {
            ffmpeg_requested = true;
            if (i + 1 < parsed.value().remaining.size() &&
                !parsed.value().remaining[i + 1].starts_with("--"))
            {
                ffmpeg_stream = parsed.value().remaining[++i];
            }
        }
        else if (argument == "--ffmpeg")
        {
            ffmpeg_executable = parsed.value().remaining[++i];
        }
        else if (argument == "--allow-remote")
        {
            server.allow_remote = true;
        }
        else if (argument == "--diagnose-input")
        {
            diagnostic = true;
        }
        else if (argument == "--confirm-movement")
        {
            confirmed = true;
        }
        else
        {
            std::cerr << "Unknown argument: " << argument << '\n';
            return 2;
        }
    }
    // A real camera needs exactly one address and an explicit --confirm-movement.
    if (!diagnostic && ((url.empty() == address.empty()) || !confirmed))
    {
        usage(argv[0]);
        return 2;
    }
    const bool secure = !server.certificate_file.empty();
    const std::string scheme = secure ? "https" : "http";
    // The page may only be used from the server's own address, unless --origin adds others.
    if (server.protocol.allowed_origins.empty())
    {
        server.protocol.allowed_origins.push_back(
            scheme + "://127.0.0.1:" + std::to_string(server.port)
        );
        server.protocol.allowed_origins.push_back(
            scheme + "://localhost:" + std::to_string(server.port)
        );
    }
    std::shared_ptr<Runtime> runtime;
    std::shared_ptr<DeviceSession> device_session;
    std::shared_ptr<PtzClient> ptz;
    std::shared_ptr<PtzController> controller;
    std::optional<Credentials> camera_credentials;
    std::string connected_device_url;
    std::vector<BrowserProfile> browser_profiles;
    // ---- Connect, and learn what each profile can do ---------------------------------
    if (!diagnostic)
    {
        auto credentials = wlonvif::examples::load_credentials(common);
        if (!credentials)
        {
            std::cerr << credentials.error().message << '\n';
            return 2;
        }
        auto made_runtime = Runtime::create();
        if (!made_runtime)
        {
            return 1;
        }
        runtime = made_runtime.value();
        auto endpoint = wlonvif::examples::resolve_device_endpoint(
            url, address, runtime, common.timeout_seconds
        );
        if (!endpoint)
        {
            std::cerr << endpoint.error().message << '\n';
            return 1;
        }
        connected_device_url = endpoint.value().url;
        auto effective_credentials = credentials.value();
        auto connected = wlonvif::examples::connect_device(
            endpoint.value().url,
            runtime,
            effective_credentials,
            common.timeout_seconds,
            {},
            &effective_credentials
        );
        if (!connected)
        {
            std::cerr << connected.error().message << '\n';
            return 1;
        }
        device_session = connected.value();
        camera_credentials = std::move(effective_credentials);
        auto made_ptz = device_session->ptz();
        if (!made_ptz)
        {
            std::cerr << made_ptz.error().message << '\n';
            return 1;
        }
        ptz = made_ptz.value();
        auto media = device_session->media();
        if (!media)
        {
            std::cerr << media.error().message << '\n';
            return 1;
        }
        auto available_profiles = wlonvif::examples::wait_for_task(
            media.value()->get_profiles(wlonvif::examples::call_options(common.timeout_seconds))
        );
        if (!available_profiles || available_profiles.value().empty())
        {
            std::cerr << (available_profiles ? "Camera returned no media profiles"
                                             : available_profiles.error().message)
                      << '\n';
            return 1;
        }
        // The page enables only the controls a profile supports.
        for (const auto& available : available_profiles.value())
        {
            BrowserProfile item{available.token, available.name};
            auto spaces = wlonvif::examples::wait_for_task(ptz->spaces(
                available.token, wlonvif::examples::call_options(common.timeout_seconds)
            ));
            if (spaces)
            {
                item.continuous_pan_tilt = !spaces.value().continuous_pan_tilt.empty();
                item.continuous_zoom = !spaces.value().continuous_zoom.empty();
                item.relative_pan_tilt = !spaces.value().relative_pan_tilt.empty();
                item.relative_zoom = !spaces.value().relative_zoom.empty();
            }
            auto presets = wlonvif::examples::wait_for_task(ptz->get_presets(
                available.token, wlonvif::examples::call_options(common.timeout_seconds)
            ));
            item.presets = presets && !presets.value().empty();
            browser_profiles.push_back(std::move(item));
        }
        if (profile.empty())
        {
            profile = browser_profiles.front().token;
        }
        if (std::ranges::none_of(
                browser_profiles,
                [&](const auto& item)
                {
                    return item.token == profile;
                }
            ))
        {
            std::cerr << "Selected profile was not returned by the camera\n";
            return 2;
        }
        auto made_controller = PtzController::create(ptz, profile);
        if (!made_controller)
        {
            std::cerr << made_controller.error().message << '\n';
            return 1;
        }
        controller = made_controller.value();
    }
    else
    {
        // Diagnostic mode: a pretend profile that supports everything, so every control can be
        // tried.
        if (profile.empty())
        {
            profile = "diagnostic";
        }
        browser_profiles.push_back({profile, "Diagnostic", true, true, true, true, true});
    }
    // ---- The profile list placed into the page ---------------------------------------
    server.ui_configuration = "{\"profiles\":[";
    for (std::size_t index = 0; index < browser_profiles.size(); ++index)
    {
        const auto& item = browser_profiles[index];
        if (index)
        {
            server.ui_configuration += ',';
        }
        server.ui_configuration +=
            "{\"token\":\"" + script_json_string(item.token) + "\",\"name\":\"" +
            script_json_string(item.name) +
            "\",\"continuous_pan_tilt\":" + (item.continuous_pan_tilt ? "true" : "false") +
            ",\"continuous_zoom\":" + (item.continuous_zoom ? "true" : "false") +
            ",\"relative_pan_tilt\":" + (item.relative_pan_tilt ? "true" : "false") +
            ",\"relative_zoom\":" + (item.relative_zoom ? "true" : "false") +
            ",\"presets\":" + (item.presets ? "true" : "false") + '}';
    }
    server.ui_configuration += "],\"selected\":\"" + script_json_string(profile) + "\"}";
    // ---- Optional video bridge ---------------------------------------------------------
    // Without a URL, use the profile's own stream, but only if it is on the same host:
    // the camera credentials are added to it, and must not go anywhere else.
    if (ffmpeg_requested && ffmpeg_stream.empty())
    {
        if (!device_session)
        {
            std::cerr << "Automatic FFmpeg stream discovery requires a camera connection\n";
            return 2;
        }
        auto media = device_session->media();
        if (!media)
        {
            std::cerr << media.error().message << '\n';
            return 1;
        }
        auto discovered = wlonvif::examples::wait_for_task(media.value()->get_stream_uri(
            profile, wlonvif::examples::call_options(common.timeout_seconds)
        ));
        if (!discovered)
        {
            std::cerr << discovered.error().message << '\n';
            return 1;
        }
        if (!discovered.value().uri.starts_with("rtsp://") &&
            !discovered.value().uri.starts_with("rtsps://"))
        {
            std::cerr << "The selected profile did not advertise an RTSP stream URI\n";
            return 1;
        }
        if (url_host(discovered.value().uri).empty() ||
            url_host(discovered.value().uri) != url_host(connected_device_url))
        {
            std::cerr << "The advertised stream host differs from the connected camera; "
                         "use --ffmpeg-stream URL to approve an explicit override\n";
            return 1;
        }
        ffmpeg_stream =
            wlonvif::examples::url_with_credentials(discovered.value().uri, camera_credentials);
        std::cout << "Discovered the selected profile's RTSP stream URI.\n";
    }
    std::shared_ptr<FfmpegVideo> ffmpeg_video;
    if (ffmpeg_requested)
    {
        std::cerr << "Warning: the FFmpeg input URI may be visible to other processes "
                     "running as this operating-system user.\n";
        auto started = FfmpegVideo::start(ffmpeg_stream, ffmpeg_executable);
        if (!started)
        {
            std::cerr << started.error().message << '\n';
            return 2;
        }
        ffmpeg_video = started.value();
        server.video = ffmpeg_video;
    }

    // ---- Optional viewport calibration, for click-to-centre and box zoom -----------------
    ViewportCalibration viewport_calibration;
    if (!calibration_file.empty())
    {
        std::ifstream input(calibration_file);
        std::ostringstream contents;
        contents << input.rdbuf();
        if (!input || contents.str().empty())
        {
            std::cerr << "Unable to read viewport calibration: " << calibration_file << '\n';
            return 2;
        }
        auto parsed_calibration = parse_viewport_calibration(contents.str());
        if (!parsed_calibration)
        {
            std::cerr << parsed_calibration.error().message << '\n';
            return 2;
        }
        viewport_calibration = parsed_calibration.value();
    }
    // ---- Serve until Ctrl-C --------------------------------------------------------------
    CameraControl control({
        ptz,
        controller,
        profile,
        browser_profiles,
        viewport_calibration,
        common.timeout_seconds,
        &stop_requested,
    });
    CommandHandler handler = [&](const WebCommand& command)
    {
        return control.handle(command);
    };
    DisconnectHandler disconnected = [&](std::string_view owner)
    {
        control.disconnected(owner);
    };

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::cout << "PTZ web UI: " << scheme << "://" << server.address << ':' << server.port << "/\n"
              << (diagnostic ? "Diagnostic mode; no camera commands will be sent.\n"
                             : "LIVE CAMERA CONTROL ENABLED.\n");
    WebServer web(std::move(server), std::move(handler), std::move(disconnected));
    auto result = web.run(stop_requested);
    // The camera is never left moving: the final close sends Stop.
    if (auto closed = control.shutdown(); !closed)
    {
        std::cerr << "Final Stop failed: " << closed.error().message << '\n';
    }
    if (runtime)
    {
        (void)runtime->shutdown();
    }
    if (!result)
    {
        std::cerr << result.error().message << '\n';
        return 1;
    }
}

/**
 * @file main.cpp
 * @brief wlonvif-explorer: find cameras, try every wlonvif feature on them, and check
 * compatibility.
 *
 *     wlonvif-explorer                      then open http://127.0.0.1:8090/
 *     wlonvif-explorer --demo               with a simulated camera, no hardware needed
 *
 * This file only reads the options and starts the server; the Explorer class
 * (explorer.hpp) holds the API, and web/ holds the page. FFmpeg, when found,
 * provides live video and recording.
 */

#include "demo.hpp"
#include "explorer.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <example_support.hpp>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace
{
    std::atomic_bool stop_requested{};
    void signal_handler(int)
    {
        stop_requested.store(true);
    }

    void usage(const char* program)
    {
        std::cout
            << "Usage: " << program << " [options]\n\n"
            << "Serve a browser UI that discovers ONVIF cameras and exercises wlonvif on them:\n"
            << "a compatibility report, live video, PTZ, imaging, analytics, events, device\n"
            << "settings, recordings, and raw requests, with full error details.\n\n"
            << "Server:\n"
            << "  --listen ADDRESS     Listen address (default 127.0.0.1)\n"
            << "  --port PORT          Listen port (default 8090)\n"
            << "  --allow-remote       Permit non-loopback serving (requires TLS)\n"
            << "  --tls-cert FILE      PEM certificate chain\n"
            << "  --tls-key FILE       PEM private key\n"
            << "  --allow-host HOST    Also accept this Host header, such as camera-lab:8090\n\n"
            << "Cameras:\n"
            << "  --username NAME --password PASSWORD | --password-env NAME | --password-file "
               "FILE\n"
            << "                       Default credentials for cameras (changeable in the UI)\n"
            << "  --ip ADDRESS         Add a camera at startup (repeatable)\n"
            << "  --url URL            Add a camera by Device service URL (repeatable)\n"
            << "  --scan               Scan the local networks at startup\n"
            << "  --demo               Add a simulated camera, to try the explorer without one\n"
            << "  --timeout SECONDS    Per-request ONVIF deadline (default 10)\n\n"
            << "Video and recording:\n"
            << "  --ffmpeg PATH        FFmpeg executable (default: ffmpeg from PATH)\n"
            << "  --no-ffmpeg          Disable live video, stream checks, and recording\n"
            << "  --recordings DIR     Where recordings are written (default "
               "./wlonvif-recordings)\n\n"
            << "Only loopback is served by default. Camera passwords are held in memory only.\n";
    }

    /// Find @p name on PATH, or return it unchanged when it already has a directory.
    std::string find_executable(const std::string& name)
    {
        if (name.find('/') != std::string::npos)
        {
            return std::filesystem::exists(name) ? name : std::string{};
        }
        const char* path = std::getenv("PATH");
        std::string_view rest = path ? path : "";
        while (!rest.empty())
        {
            const auto end = rest.find(':');
            const auto candidate = std::filesystem::path(rest.substr(0, end)) / name;
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error))
            {
                return candidate.string();
            }
            if (end == std::string_view::npos)
            {
                break;
            }
            rest.remove_prefix(end + 1);
        }
        return {};
    }
} // namespace

int main(int argc, char** argv)
{
    using namespace wlonvif;
    using namespace wlonvif::examples;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string_view(argv[i]) == "--help" || std::string_view(argv[i]) == "-h")
        {
            usage(argv[0]);
            return 0;
        }
    }
    auto parsed = parse_common_arguments(argc, argv);
    if (!parsed)
    {
        std::cerr << parsed.error().message << '\n';
        return 2;
    }
    auto common = parsed.value().common;
    const auto& arguments = parsed.value().remaining;
    web::ServerOptions server;
    server.port = 8090;
    server.body_limit = 256 * 1024;
    server.maximum_connections = 32;
    // Compatibility checks and scans answer slowly; only stalled writes are bounded.
    server.write_timeout = std::chrono::seconds(30);
    server.server_name = "wlonvif-explorer";
    explorer::ExplorerOptions options;
    std::string ffmpeg = "ffmpeg";
    bool scan = false, demo = false;
    options.recordings = "wlonvif-recordings";
    std::vector<std::string> addresses, urls, extra_hosts;
    for (std::size_t i = 0; i < arguments.size(); ++i)
    {
        const std::string_view argument(arguments[i]);
        const bool takes =
            argument == "--listen" || argument == "--port" || argument == "--tls-cert" ||
            argument == "--tls-key" || argument == "--allow-host" || argument == "--ip" ||
            argument == "--url" || argument == "--ffmpeg" || argument == "--recordings";
        if (takes && i + 1 >= arguments.size())
        {
            std::cerr << "Missing value for " << argument << '\n';
            return 2;
        }
        if (argument == "--listen")
        {
            server.address = arguments[++i];
        }
        else if (argument == "--port")
        {
            std::size_t port = 0;
            if (!parse_size(arguments[++i], port) || port > 65535)
            {
                std::cerr << "Invalid port\n";
                return 2;
            }
            server.port = static_cast<std::uint16_t>(port);
        }
        else if (argument == "--allow-remote")
        {
            server.allow_remote = true;
        }
        else if (argument == "--tls-cert")
        {
            server.certificate_file = arguments[++i];
        }
        else if (argument == "--tls-key")
        {
            server.private_key_file = arguments[++i];
        }
        else if (argument == "--allow-host")
        {
            extra_hosts.push_back(arguments[++i]);
        }
        else if (argument == "--ip")
        {
            addresses.push_back(arguments[++i]);
        }
        else if (argument == "--url")
        {
            urls.push_back(arguments[++i]);
        }
        else if (argument == "--scan")
        {
            scan = true;
        }
        else if (argument == "--demo")
        {
            demo = true;
        }
        else if (argument == "--ffmpeg")
        {
            ffmpeg = arguments[++i];
        }
        else if (argument == "--no-ffmpeg")
        {
            ffmpeg.clear();
        }
        else if (argument == "--recordings")
        {
            options.recordings = arguments[++i];
        }
        else
        {
            std::cerr << "Unknown argument: " << argument << '\n';
            usage(argv[0]);
            return 2;
        }
    }
    if (!ffmpeg.empty())
    {
        options.ffmpeg = find_executable(ffmpeg);
        if (options.ffmpeg.empty())
        {
            std::cerr << "note: FFmpeg not found (" << ffmpeg
                      << "); live video and recording are disabled\n";
        }
    }
    if (options.ffmpeg.empty())
    {
        options.recordings.clear();
    }
    if (!common.username.empty())
    {
        auto credentials = load_credentials(common);
        if (!credentials)
        {
            std::cerr << credentials.error().message << '\n';
            return 2;
        }
        options.default_credentials = credentials.value();
    }
    options.timeout_seconds = common.timeout_seconds;
    const auto port = std::to_string(server.port);
    options.allowed_hosts = {
        "127.0.0.1:" + port, "localhost:" + port, "[::1]:" + port, server.address + ":" + port
    };
    if (server.port == 80 || server.port == 443)
    {
        options.allowed_hosts.insert(
            options.allowed_hosts.end(), {"127.0.0.1", "localhost", "[::1]", server.address}
        );
    }
    options.allowed_hosts.insert(
        options.allowed_hosts.end(), extra_hosts.begin(), extra_hosts.end()
    );

    auto runtime_result = Runtime::create();
    if (!runtime_result)
    {
        std::cerr << runtime_result.error().message << '\n';
        return 1;
    }
    auto runtime = runtime_result.value();
    const int timeout = options.timeout_seconds;
    auto demo_camera = demo ? explorer::make_demo_camera(runtime) : nullptr;
    if (demo)
    {
        urls.push_back(explorer::demo_url);
    }
    auto explorer = std::make_shared<explorer::Explorer>(
        options,
        runtime,
        [runtime, timeout, demo_camera](
            const std::string& url, const std::optional<Credentials>& credentials
        ) -> Result<std::shared_ptr<DeviceSession>>
        {
            if (demo_camera && url == explorer::demo_url)
            {
                return wait_for_task(
                    DeviceSession::connect(
                        url,
                        demo_camera,
                        make_same_host_policy(),
                        runtime,
                        credentials,
                        call_options(timeout)
                    )
                );
            }
            return connect_device(url, runtime, credentials, timeout);
        },
        [runtime](NetworkDiscoveryOptions discovery, CallOptions call)
        {
            return wait_for_task(
                discover_network(runtime, make_same_host_policy(), std::move(discovery), call)
            );
        },
        [runtime, timeout](const std::string& address) -> Result<std::string>
        {
            auto resolved = resolve_device_endpoint("", address, runtime, timeout);
            if (!resolved)
            {
                return resolved.error();
            }
            return resolved.value().url;
        }
    );

    // The shared example helpers install their own interrupt handler the first
    // time they are used; initialise them now so the server's handler wins.
    (void)call_options(0);
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    const auto scheme = server.certificate_file.empty() ? "http://" : "https://";
    std::cout << "wlonvif explorer: open " << scheme
              << (server.address == "0.0.0.0" ? std::string("127.0.0.1") : server.address) << ':'
              << server.port << "/ in a browser (Ctrl-C to stop)\n";
    if (!options.ffmpeg.empty())
    {
        std::cout << "FFmpeg: " << options.ffmpeg << "\n";
    }
    // Startup cameras and scans go through the same API the browser uses, so
    // they behave identically and show up in its request log.
    std::thread startup;
    if (scan || !addresses.empty() || !urls.empty())
    {
        startup = std::thread(
            [&, explorer]
            {
                web::Request request;
                request.method = "POST";
                request.host = "127.0.0.1:" + port;
                request.origin = "http://" + request.host;
                request.cookie = "wlonvif_explorer=" + explorer->session_token();
                for (const auto& address : addresses)
                {
                    request.target = "/api/cameras";
                    request.body = "{\"address\":" + web::json_string(address) + "}";
                    (void)explorer->handle(request);
                }
                for (const auto& url : urls)
                {
                    request.target = "/api/cameras";
                    // The simulated camera accepts any login, so it starts signed in.
                    request.body =
                        "{\"url\":" + web::json_string(url) +
                        (url == explorer::demo_url ? ",\"username\":\"demo\",\"password\":\"demo\"}"
                                                   : "}");
                    (void)explorer->handle(request);
                }
                if (scan)
                {
                    request.target = "/api/scan";
                    request.body = "{}";
                    (void)explorer->handle(request);
                }
            }
        );
    }
    // The simulated camera reports motion now and then, so the Events tab has data.
    std::thread demo_events;
    if (demo_camera)
    {
        demo_events = std::thread(
            [&, demo_camera]
            {
                for (int tick = 0; !stop_requested.load(); ++tick)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    if (tick % 20 != 0)
                    {
                        continue;
                    }
                    Event event;
                    event.topic = "tns1:RuleEngine/CellMotionDetector/Motion";
                    event.utc_time = std::chrono::system_clock::now();
                    event.source = "VideoSourceConfigurationToken=source-config,Rule=MotionRule";
                    event.data = tick % 40 == 0 ? "IsMotion=true" : "IsMotion=false";
                    demo_camera->emit(event);
                }
            }
        );
    }
    auto served = web::Server(server, explorer).run(stop_requested);
    stop_requested.store(true);
    if (demo_events.joinable())
    {
        demo_events.join();
    }
    if (startup.joinable())
    {
        startup.join();
    }
    explorer.reset();
    (void)runtime->shutdown();
    if (!served)
    {
        std::cerr << "Server failed: " << served.error().message << '\n';
        return 1;
    }
    return 0;
}

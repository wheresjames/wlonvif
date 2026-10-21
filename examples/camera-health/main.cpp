/**
 * @file main.cpp
 * @brief wlonvif-camera-health: a quick OK/WARN report on whether a camera is usable.
 *
 *     wlonvif-camera-health --ip 192.0.2.10 --username admin
 *
 * Checks, in order: finding the Device service, connecting, identity, clock
 * (more than 5 minutes off breaks WS-Security sign-in on many cameras),
 * advertised services, a media profile with a stream URI, whether PTZ is
 * offered, and whether events can be read. Suited to monitoring scripts:
 * --json prints one array. The exit status is 1 only when the device cannot be
 * found or connected to; other problems are reported as WARN lines.
 */

#include <cmath>
#include <example_support.hpp>
#include <iostream>

namespace
{
    /// One line of the report.
    struct Check
    {
        std::string name;
        bool healthy{};
        std::string detail;
    };

    void print_checks(const std::vector<Check>& checks, bool json)
    {
        if (json)
        {
            std::cout << '[';
        }
        for (std::size_t index = 0; index < checks.size(); ++index)
        {
            const auto& check = checks[index];
            if (json)
            {
                std::cout << (index ? "," : "") << "{\"check\":\""
                          << wlonvif::examples::json_escape(check.name)
                          << "\",\"healthy\":" << (check.healthy ? "true" : "false")
                          << ",\"detail\":\"" << wlonvif::examples::json_escape(check.detail)
                          << "\"}";
            }
            else
            {
                std::cout << (check.healthy ? "OK    " : "WARN  ") << check.name << ": "
                          << check.detail << '\n';
            }
        }
        if (json)
        {
            std::cout << "]\n";
        }
    }
} // namespace

int main(int argc, char** argv)
{
    auto parsed = wlonvif::examples::parse_common_arguments(argc, argv);
    if (!parsed)
    {
        std::cerr << parsed.error().message << '\n';
        return 2;
    }
    auto common = std::move(parsed.value().common);
    std::string url, address;
    for (std::size_t index = 0; index < parsed.value().remaining.size(); ++index)
    {
        const auto& argument = parsed.value().remaining[index];
        if (argument == "--help" || argument == "-h")
        {
            std::cout << "Usage: " << argv[0]
                      << " (--url DEVICE_URL | --ip ADDRESS) [common options]\n"
                      << "Credentials: --username NAME --password PASSWORD; environment and"
                         " prompt fallbacks apply.\n";
            return 0;
        }
        if (argument == "--url" && index + 1 < parsed.value().remaining.size())
        {
            url = parsed.value().remaining[++index];
        }
        else if (argument == "--ip" && index + 1 < parsed.value().remaining.size())
        {
            address = parsed.value().remaining[++index];
        }
        else
        {
            std::cerr << "Unknown or incomplete argument: " << argument << '\n';
            return 2;
        }
    }
    if (url.empty() == address.empty())
    {
        std::cerr << "Specify exactly one of --url or --ip\n";
        return 2;
    }
    auto credentials = wlonvif::examples::load_credentials(common);
    if (!credentials)
    {
        std::cerr << credentials.error().message << '\n';
        return 2;
    }
    auto created = wlonvif::Runtime::create();
    if (!created)
    {
        std::cerr << created.error().message << '\n';
        return 1;
    }
    auto runtime = created.value();
    auto endpoint =
        wlonvif::examples::resolve_device_endpoint(url, address, runtime, common.timeout_seconds);
    if (!endpoint)
    {
        print_checks({{"discovery", false, endpoint.error().message}}, common.json);
        (void)runtime->shutdown();
        return 1;
    }
    const auto discovery_evidence = endpoint.value().evidence;
    url = std::move(endpoint.value().url);
    auto connected = wlonvif::examples::connect_device(
        url, runtime, credentials.value(), common.timeout_seconds
    );
    if (!connected)
    {
        print_checks({{"device", false, connected.error().message}}, common.json);
        (void)runtime->shutdown();
        return 1;
    }
    auto device = connected.value();
    std::vector<Check> checks{
        {"discovery", true, discovery_evidence + ": " + wlonvif::examples::redact_url(url)},
        {"device", true, "Device service responded"}
    };
    auto information = wlonvif::examples::wait_for_task(
        device->get_device_information(wlonvif::examples::call_options(common.timeout_seconds))
    );
    checks.push_back(
        {"identity",
         static_cast<bool>(information),
         information ? information.value().manufacturer + " " + information.value().model
                     : information.error().message}
    );
    // WS-Security signatures carry a timestamp, so a far-off clock can make
    // the camera reject every authenticated request.
    auto clock = wlonvif::examples::wait_for_task(
        device->get_system_date_and_time(wlonvif::examples::call_options(common.timeout_seconds))
    );
    if (clock)
    {
        const auto skew = std::llabs(
            std::chrono::duration_cast<std::chrono::seconds>(
                clock.value().utc - std::chrono::system_clock::now()
            )
                .count()
        );
        checks.push_back({"clock", skew <= 300, "absolute skew " + std::to_string(skew) + "s"});
    }
    else
    {
        checks.push_back({"clock", false, clock.error().message});
    }
    auto services = wlonvif::examples::wait_for_task(
        device->get_services(wlonvif::examples::call_options(common.timeout_seconds))
    );
    checks.push_back(
        {"services",
         static_cast<bool>(services),
         services ? std::to_string(services.value().size()) + " advertised"
                  : services.error().message}
    );
    // A stream URI for the first profile shows Media works end to end.
    auto media = device->media();
    wlonvif::ProfileToken profile;
    if (media)
    {
        auto profiles = wlonvif::examples::wait_for_task(
            media.value()->get_profiles(wlonvif::examples::call_options(common.timeout_seconds))
        );
        if (profiles && !profiles.value().empty())
        {
            profile = profiles.value().front().token;
            auto uri = wlonvif::examples::wait_for_task(media.value()->get_stream_uri(
                profile, wlonvif::examples::call_options(common.timeout_seconds)
            ));
            checks.push_back(
                {"media",
                 static_cast<bool>(uri),
                 uri ? "profile and stream URI available" : uri.error().message}
            );
        }
        else
        {
            checks.push_back({"media", false, profiles ? "no profiles" : profiles.error().message});
        }
    }
    else
    {
        checks.push_back({"media", false, media.error().message});
    }
    auto capabilities = wlonvif::examples::wait_for_task(
        device->get_capabilities(wlonvif::examples::call_options(common.timeout_seconds))
    );
    checks.push_back(
        {"ptz",
         capabilities && capabilities.value().ptz_xaddr.has_value(),
         capabilities && capabilities.value().ptz_xaddr ? "advertised" : "not advertised"}
    );
    // One short poll proves the event subscription can be created and read.
    auto events = device->events(8);
    if (events)
    {
        auto poll = wlonvif::examples::wait_for_task(
            events.value()->poll(1, wlonvif::examples::call_options(common.timeout_seconds))
        );
        checks.push_back(
            {"events",
             static_cast<bool>(poll),
             poll ? "PullPoint responsive" : poll.error().message}
        );
        (void)wlonvif::examples::wait_for_task(
            events.value()->close(wlonvif::examples::safety_call_options(3))
        );
    }
    else
    {
        checks.push_back({"events", false, events.error().message});
    }
    print_checks(checks, common.json);
    const bool device_failed = !checks[1].healthy;
    (void)runtime->shutdown();
    return device_failed ? 1 : 0;
}

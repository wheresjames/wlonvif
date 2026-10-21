/**
 * @file main.cpp
 * @brief wlonvif-device-config: show and change a device's clock, NTP, network, and scopes.
 *
 *     wlonvif-device-config --ip 192.0.2.10 --username admin
 *     wlonvif-device-config --ip 192.0.2.10 --username admin --sync-time --location "Front door"
 *
 * Without changes it prints the configuration. With changes it applies them in
 * order, reporting each one as it goes, then reads everything back: cameras
 * often accept a value and store something else (only one NTP server, say),
 * and the read-back makes that visible. Reading and printing are in
 * configuration.cpp; this file handles the options and the changes.
 */

#include "configuration.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <example_support.hpp>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    void usage(const char* executable)
    {
        std::cout
            << "Usage: " << executable
            << " (--url URL | --ip ADDRESS) [changes] [common options]\n\n"
            << "With no changes, show the clock, NTP, and scope configuration.\n\n"
            << "Clock:\n"
            << "  --sync-time            Set the clock manually to this computer's UTC time\n"
            << "  --ntp                  Keep the clock synchronized from NTP\n"
            << "  --ntp-server HOST      Use this NTP server (repeatable; implies --ntp)\n"
            << "  --ntp-dhcp             Take NTP servers from DHCP (implies --ntp)\n"
            << "  --time-zone TZ         POSIX time zone, e.g. CET-1CEST,M3.5.0,M10.5.0/3\n"
            << "  --dst on|off           Whether daylight saving applies\n\n"
            << "Network:\n"
            << "  --hostname NAME        Set the device hostname\n"
            << "  --discoverable on|off  Whether the device answers WS-Discovery probes\n\n"
            << "Maintenance:\n"
            << "  --reboot --confirm-reboot  Restart the device\n\n"
            << "Scopes:\n"
            << "  --location TEXT        Replace the configurable location scope\n"
            << "  --add-scope URI        Add a configurable scope (repeatable)\n"
            << "  --remove-scope URI     Remove a configurable scope (repeatable)\n\n"
            << "Common options:\n"
            << "  --timeout SECONDS --username NAME --password PASSWORD --password-file FILE\n"
            << "  --password-env NAME --allow-cleartext-basic --json --help\n\n"
            << "Changes require administrator credentials. Fixed scopes are built into\n"
            << "the device and cannot be changed or removed.\n";
    }
} // namespace

using namespace device_config;

int main(int argc, char** argv)
{
    auto parsed = wlonvif::examples::parse_common_arguments(argc, argv);
    if (!parsed)
    {
        std::cerr << parsed.error().message << '\n';
        return 2;
    }
    auto common = std::move(parsed.value().common);
    const auto& arguments = parsed.value().remaining;
    std::string url, address;
    bool sync_time = false, ntp = false, ntp_dhcp = false;
    std::vector<std::string> ntp_servers, add_scopes, remove_scopes;
    std::optional<std::string> time_zone, location;
    std::optional<bool> dst, discoverable;
    std::optional<std::string> hostname;
    bool reboot = false, confirm_reboot = false;
    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::string_view argument(arguments[index]);
        const bool needs_value = argument == "--url" || argument == "--ip" ||
                                 argument == "--ntp-server" || argument == "--time-zone" ||
                                 argument == "--dst" || argument == "--location" ||
                                 argument == "--add-scope" || argument == "--remove-scope" ||
                                 argument == "--hostname" || argument == "--discoverable";
        if (argument == "--help" || argument == "-h")
        {
            usage(argv[0]);
            return 0;
        }
        if (needs_value && index + 1 >= arguments.size())
        {
            std::cerr << "Missing value for " << argument << '\n';
            return 2;
        }
        if (argument == "--url")
        {
            url = arguments[++index];
        }
        else if (argument == "--ip")
        {
            address = arguments[++index];
        }
        else if (argument == "--sync-time")
        {
            sync_time = true;
        }
        else if (argument == "--ntp")
        {
            ntp = true;
        }
        else if (argument == "--ntp-dhcp")
        {
            ntp = ntp_dhcp = true;
        }
        else if (argument == "--ntp-server")
        {
            ntp = true;
            ntp_servers.push_back(arguments[++index]);
        }
        else if (argument == "--time-zone")
        {
            time_zone = arguments[++index];
        }
        else if (argument == "--dst")
        {
            const std::string_view value(arguments[++index]);
            if (value != "on" && value != "off")
            {
                std::cerr << "--dst takes on or off\n";
                return 2;
            }
            dst = value == "on";
        }
        else if (argument == "--location")
        {
            location = arguments[++index];
        }
        else if (argument == "--hostname")
        {
            hostname = arguments[++index];
        }
        else if (argument == "--discoverable")
        {
            const std::string_view value(arguments[++index]);
            if (value != "on" && value != "off")
            {
                std::cerr << "--discoverable takes on or off\n";
                return 2;
            }
            discoverable = value == "on";
        }
        else if (argument == "--reboot")
        {
            reboot = true;
        }
        else if (argument == "--confirm-reboot")
        {
            confirm_reboot = true;
        }
        else if (argument == "--add-scope")
        {
            add_scopes.push_back(arguments[++index]);
        }
        else if (argument == "--remove-scope")
        {
            remove_scopes.push_back(arguments[++index]);
        }
        else
        {
            std::cerr << "Unknown argument: " << argument << '\n';
            usage(argv[0]);
            return 2;
        }
    }
    if (url.empty() == address.empty())
    {
        std::cerr << "Specify exactly one of --url or --ip\n";
        return 2;
    }
    if (sync_time && ntp)
    {
        std::cerr
            << "--sync-time sets the clock manually; it cannot be combined with NTP options\n";
        return 2;
    }
    if (ntp_dhcp && !ntp_servers.empty())
    {
        std::cerr << "--ntp-dhcp and --ntp-server are alternatives\n";
        return 2;
    }
    if (location && location->empty())
    {
        std::cerr << "--location needs a non-empty value\n";
        return 2;
    }
    const bool clock_change = sync_time || ntp || time_zone || dst;
    const bool scope_change = location || !add_scopes.empty() || !remove_scopes.empty();
    const bool network_change = hostname || discoverable;
    if (reboot && !confirm_reboot)
    {
        std::cerr << "--reboot restarts the device; add --confirm-reboot to proceed\n";
        return 2;
    }

    auto credentials = wlonvif::examples::load_credentials(common);
    if (!credentials)
    {
        std::cerr << credentials.error().message << '\n';
        return 2;
    }
    auto runtime_result = wlonvif::Runtime::create();
    if (!runtime_result)
    {
        std::cerr << runtime_result.error().message << '\n';
        return 1;
    }
    auto runtime = runtime_result.value();
    const auto finish = [&](int status)
    {
        (void)runtime->shutdown();
        return status;
    };
    auto endpoint =
        wlonvif::examples::resolve_device_endpoint(url, address, runtime, common.timeout_seconds);
    if (!endpoint)
    {
        std::cerr << "Endpoint discovery failed: " << endpoint.error().message << '\n';
        return finish(1);
    }
    auto connected = wlonvif::examples::connect_device(
        endpoint.value().url, runtime, std::move(credentials.value()), common.timeout_seconds
    );
    if (!connected)
    {
        std::cerr << "Connection failed: " << refusal_text(connected.error()) << '\n';
        return finish(1);
    }
    auto device = connected.value();
    using wlonvif::examples::call_options;
    using wlonvif::examples::wait_for_task;
    const auto timeout = common.timeout_seconds;

    auto before = read_configuration(device, timeout);
    if (!before)
    {
        std::cerr << "Query failed: " << refusal_text(before.error()) << '\n';
        return finish(1);
    }
    if (!clock_change && !scope_change && !network_change && !reboot)
    {
        print_configuration(before.value(), common.json);
        return finish(0);
    }

    // Each change is reported as it is applied, so a refusal part-way through
    // leaves a clear record of what did and did not take effect.
    int status = 0;
    const auto apply = [&](std::string_view description, wlonvif::Result<void> result)
    {
        if (result)
        {
            if (!common.json)
            {
                std::cout << "  ok      " << description << '\n';
            }
            return true;
        }
        std::cerr << "  failed  " << description << ": " << refusal_text(result.error()) << '\n';
        status = 1;
        return false;
    };
    if (!common.json)
    {
        std::cout << "Applying changes\n";
    }

    if (ntp && (ntp_dhcp || !ntp_servers.empty()))
    {
        wlonvif::NtpConfiguration configuration;
        configuration.from_dhcp = ntp_dhcp;
        configuration.servers = ntp_servers;
        apply(
            ntp_dhcp ? "NTP servers from DHCP" : "NTP servers " + joined(ntp_servers),
            wait_for_task(device->set_ntp(configuration, call_options(timeout)))
        );
    }
    if (clock_change && status == 0)
    {
        const auto& current = before.value().clock;
        wlonvif::SystemDateAndTimeSettings settings;
        settings.type = sync_time ? wlonvif::DateTimeType::manual
                        : ntp     ? wlonvif::DateTimeType::ntp
                                  : current.type;
        settings.daylight_savings = dst.value_or(current.daylight_savings);
        settings.time_zone = time_zone;
        std::string description =
            settings.type == wlonvif::DateTimeType::ntp ? "clock mode NTP" : "clock mode manual";
        if (sync_time)
        {
            settings.utc = std::chrono::system_clock::now();
            description += ", time " + utc_text(*settings.utc);
        }
        else if (settings.type == wlonvif::DateTimeType::manual)
        {
            // A manual clock needs a time with every change; keep the device's
            // own time rather than silently replacing it with this computer's.
            settings.utc = std::chrono::system_clock::now() + before.value().skew;
        }
        if (time_zone)
        {
            description += ", time zone " + *time_zone;
        }
        if (dst)
        {
            description += std::string(", daylight saving ") + (*dst ? "on" : "off");
        }
        apply(
            description,
            wait_for_task(
                device->set_system_date_and_time(std::move(settings), call_options(timeout))
            )
        );
    }
    if (location)
    {
        std::vector<std::string> stale;
        bool fixed_location = false;
        for (const auto& scope : before.value().scopes)
        {
            if (!scope.uri.starts_with(LocationPrefix))
            {
                continue;
            }
            if (scope.definition == wlonvif::ScopeDefinition::configurable)
            {
                stale.push_back(scope.uri);
            }
            else
            {
                fixed_location = true;
            }
        }
        const auto replacement = std::string(LocationPrefix) + percent_encode(*location);
        std::erase(stale, replacement);
        if (!stale.empty())
        {
            apply(
                "remove previous location",
                wait_for_task(device->remove_scopes(stale, call_options(timeout)))
            );
        }
        if (std::none_of(
                before.value().scopes.begin(),
                before.value().scopes.end(),
                [&](const auto& scope)
                {
                    return scope.uri == replacement;
                }
            ))
        {
            apply(
                "location " + *location,
                wait_for_task(device->add_scopes({replacement}, call_options(timeout)))
            );
        }
        if (fixed_location && !common.json)
        {
            std::cout << "  note    the device's fixed location cannot be removed;"
                         " discovery will report both\n";
        }
    }
    if (!remove_scopes.empty())
    {
        apply(
            "remove " + joined(remove_scopes),
            wait_for_task(device->remove_scopes(remove_scopes, call_options(timeout)))
        );
    }
    if (!add_scopes.empty())
    {
        apply(
            "add " + joined(add_scopes),
            wait_for_task(device->add_scopes(add_scopes, call_options(timeout)))
        );
    }

    if (hostname)
    {
        apply(
            "hostname " + *hostname,
            wait_for_task(device->set_hostname(*hostname, call_options(timeout)))
        );
    }
    if (discoverable)
    {
        apply(
            std::string("discoverable ") + (*discoverable ? "on" : "off"),
            wait_for_task(device->set_discovery_mode(
                *discoverable ? wlonvif::DiscoveryMode::discoverable
                              : wlonvif::DiscoveryMode::non_discoverable,
                call_options(timeout)
            ))
        );
    }
    if (reboot)
    {
        // Rebooting last lets every other change be applied and reported first.
        auto message = wait_for_task(device->system_reboot(call_options(timeout)));
        if (message)
        {
            if (!common.json)
            {
                std::cout << "  ok      reboot"
                          << (message.value().empty() ? "" : ": " + message.value())
                          << "\nThe device is restarting; it will be unreachable for a while.\n";
            }
            return finish(status);
        }
        std::cerr << "  failed  reboot: " << refusal_text(message.error()) << '\n';
        return finish(1);
    }

    auto after = read_configuration(device, timeout);
    if (!after && hostname && after.error().category == wlonvif::ErrorCategory::network)
    {
        // Some devices restart their network stack to apply a new hostname.
        if (!common.json)
        {
            std::cout << "  wait    the device is restarting its network\n";
        }
        const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!after && std::chrono::steady_clock::now() < give_up)
        {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            after = read_configuration(device, timeout);
        }
    }
    if (!after)
    {
        std::cerr << "Query failed: " << refusal_text(after.error()) << '\n';
        return finish(1);
    }
    // Devices acknowledge SetNTP even when they store less than they were sent,
    // such as only one server, or a DNS name as an empty address.
    if (!ntp_servers.empty() && after.value().ntp && after.value().ntp->servers != ntp_servers)
    {
        const auto& stored = after.value().ntp->servers;
        std::cerr << "  note    the device stored NTP server(s) "
                  << (stored.empty() ? std::string("(none)") : joined(stored))
                  << "; it may accept fewer servers, or only IP addresses\n";
    }
    if (!common.json)
    {
        std::cout << '\n';
    }
    print_configuration(after.value(), common.json);
    return finish(status);
}

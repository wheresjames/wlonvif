/**
 * @file main.cpp
 * @brief wlonvif-discover: find the ONVIF devices on the local network.
 *
 *     wlonvif-discover                 WS-Discovery multicast only
 *     wlonvif-discover --scan auto     also probe every address of the local networks
 *     wlonvif-discover --ip 192.0.2.10
 *
 * WS-Discovery is quick but misses devices that do not answer multicast, or
 * whose replies a firewall drops. Direct probing tries each address on the
 * usual ONVIF ports and paths, and classifies what answered: a full ONVIF
 * response, a SOAP fault, or an authentication challenge. Each address gets
 * its own time limit, and the overall limit leaves room to finish the range;
 * addresses that still could not be checked are listed at the end.
 */

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <example_support.hpp>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <wlonvif/wlonvif.hpp>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace
{
    constexpr std::string_view Bold = "\033[1m";
    constexpr std::string_view Dim = "\033[2m";
    constexpr std::string_view Green = "\033[32m";
    constexpr std::string_view Yellow = "\033[33m";
    constexpr std::string_view Cyan = "\033[36m";
    constexpr std::string_view Reset = "\033[0m";

    /// ANSI styling that collapses to nothing when it would not render.
    struct Style
    {
        bool enabled{};
        std::string_view operator()(std::string_view code) const
        {
            return enabled ? code : std::string_view{};
        }
    };

    /// Style for @p stream: a terminal, with NO_COLOR unset and TERM not "dumb".
    Style style_for([[maybe_unused]] std::FILE* stream)
    {
#ifdef _WIN32
        // Legacy Windows consoles print escape sequences literally.
        return {};
#else
        const char* term = std::getenv("TERM");
        return {
            !std::getenv("NO_COLOR") && !(term && std::string_view(term) == "dumb") &&
            isatty(fileno(stream)) != 0
        };
#endif
    }

    std::string_view evidence_name(wlonvif::DeviceEvidence evidence)
    {
        using enum wlonvif::DeviceEvidence;
        switch (evidence)
        {
            case discovery:
                return "discovered";
            case authentication_challenge:
                return "authentication-required";
            case soap_fault:
                return "probable-onvif";
            case onvif_response:
                return "confirmed-onvif";
        }
        return "unknown";
    }

    /// Human-readable evidence label and the color that signals its strength.
    std::pair<std::string_view, std::string_view> evidence_label(wlonvif::DeviceEvidence evidence)
    {
        using enum wlonvif::DeviceEvidence;
        switch (evidence)
        {
            case onvif_response:
                return {"confirmed ONVIF", Green};
            case authentication_challenge:
                return {"ONVIF, login required", Cyan};
            case soap_fault:
                return {"probable ONVIF", Yellow};
            case discovery:
                return {"discovered, not verified", Yellow};
        }
        return {"unknown", Yellow};
    }

    std::string percent_decode(std::string_view text)
    {
        std::string output;
        for (std::size_t index = 0; index < text.size(); ++index)
        {
            unsigned value = 0;
            if (text[index] == '%' && index + 2 < text.size() &&
                std::from_chars(text.data() + index + 1, text.data() + index + 3, value, 16).ptr ==
                    text.data() + index + 3)
            {
                output.push_back(static_cast<char>(value));
                index += 2;
            }
            else
            {
                output.push_back(text[index]);
            }
        }
        return output;
    }

    /// Standard ONVIF scopes decoded into display fields; the rest kept verbatim.
    struct ScopeFields
    {
        std::vector<std::string> name, hardware, location, profiles, type, other;
    };

    ScopeFields parse_scopes(const std::vector<std::string>& scopes)
    {
        constexpr std::string_view prefix = "onvif://www.onvif.org/";
        ScopeFields fields;
        for (const auto& scope : scopes)
        {
            std::string_view rest(scope);
            const auto slash = rest.find('/', prefix.size());
            if (!rest.starts_with(prefix) || slash == rest.npos)
            {
                fields.other.push_back(scope);
                continue;
            }
            std::string category(rest.substr(prefix.size(), slash - prefix.size()));
            std::transform(
                category.begin(),
                category.end(),
                category.begin(),
                [](unsigned char c)
                {
                    return static_cast<char>(std::tolower(c));
                }
            );
            auto value = percent_decode(rest.substr(slash + 1));
            if (category == "name")
            {
                fields.name.push_back(std::move(value));
            }
            else if (category == "hardware")
            {
                fields.hardware.push_back(std::move(value));
            }
            else if (category == "location")
            {
                fields.location.push_back(std::move(value));
            }
            else if (category == "profile")
            {
                fields.profiles.push_back(std::move(value));
            }
            else if (category == "type")
            {
                fields.type.push_back(std::move(value));
            }
            else
            {
                fields.other.push_back(scope);
            }
        }
        return fields;
    }

    std::string joined(const std::vector<std::string>& values)
    {
        std::string output;
        for (const auto& value : values)
        {
            output += (output.empty() ? "" : ", ") + value;
        }
        return output;
    }

    void print_device(const wlonvif::DeviceCandidate& candidate, const Style& style)
    {
        const auto fields =
            candidate.discovery ? parse_scopes(candidate.discovery->scopes) : ScopeFields{};
        const auto [label, color] = evidence_label(candidate.evidence);
        std::cout << "  " << style(Bold) << candidate.address << style(Reset);
        if (!fields.name.empty())
        {
            std::cout << "  " << style(Bold) << joined(fields.name) << style(Reset);
        }
        std::cout << "  " << style(color) << '[' << label << ']' << style(Reset) << '\n';
        const auto field = [&](std::string_view name, const std::string& value, bool dim = false)
        {
            if (value.empty())
            {
                return;
            }
            std::cout << "      " << style(Dim) << name;
            for (auto pad = name.size(); pad < 10; ++pad)
            {
                std::cout << ' ';
            }
            std::cout << style(Reset) << (dim ? style(Dim) : "") << value << style(Reset) << '\n';
        };
        field("Service", candidate.device_service.value_or(""));
        if (fields.hardware != fields.name)
        {
            field("Hardware", joined(fields.hardware));
        }
        field("Location", joined(fields.location));
        field("Profiles", joined(fields.profiles));
        field("Type", joined(fields.type));
        for (const auto& scope : fields.other)
        {
            field("Scope", scope);
        }
        if (candidate.discovery)
        {
            field("Endpoint", candidate.discovery->endpoint_reference, true);
        }
        field("Evidence", candidate.diagnostic, true);
    }

    /// What a search covered, kept so an empty result can suggest a wider one.
    struct SearchScope
    {
        bool multicast{};
        bool scanned{};
        long address_timeout_seconds{};
    };

    /// Explain why nothing answered and print search commands that cover more.
    void empty_result_hints(const char* executable, const SearchScope& scope)
    {
        const auto style = style_for(stderr);
        std::cerr << '\n' << style(Bold) << "Possible causes:" << style(Reset) << '\n';
        if (scope.multicast)
        {
            std::cerr << "  - A local firewall may be dropping WS-Discovery replies.\n";
            std::cerr
                << "  - The network (often Wi-Fi) does not forward multicast between clients.\n";
        }
        if (scope.scanned)
        {
            std::cerr << "  - The device serves ONVIF on a port the direct probe does not try.\n"
                      << "  - The device takes longer than " << scope.address_timeout_seconds
                      << "s to answer a probe.\n";
        }
        std::cerr << '\n'
                  << style(Bold)
                  << "These ask each address directly and are unaffected by multicast filtering:"
                  << style(Reset) << '\n';
        // The bare program name reads as a command to type, unlike a build path.
        std::string_view program(executable);
        program.remove_prefix(program.find_last_of("/\\") + 1);
        const auto suggest = [&](std::string_view arguments, std::string_view purpose)
        {
            std::cerr << "  " << style(Cyan) << program << ' ' << arguments << style(Reset)
                      << "\n      " << purpose << '\n';
        };
        if (!scope.scanned)
        {
            suggest("--scan auto", "Probe every address on the local networks");
        }
        else
        {
            const auto longer = std::to_string(std::max(10L, scope.address_timeout_seconds * 2));
            suggest("--scan auto --timeout " + longer, "Give slow devices longer to answer");
        }
        suggest("--target ADDRESS", "One known device, including non-default ports it advertises");
    }

    std::optional<std::uint32_t> ipv4_value(std::string_view text)
    {
        std::uint32_t value = 0;
        for (int octet = 0; octet < 4; ++octet)
        {
            unsigned part = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), part);
            if (parsed.ec != std::errc{} || part > 255)
            {
                return std::nullopt;
            }
            text.remove_prefix(static_cast<std::size_t>(parsed.ptr - text.data()));
            if (octet < 3)
            {
                if (text.empty() || text.front() != '.')
                {
                    return std::nullopt;
                }
                text.remove_prefix(1);
            }
            value = (value << 8) | part;
        }
        return text.empty() ? std::optional<std::uint32_t>(value) : std::nullopt;
    }

    /// Collapse consecutive IPv4 addresses into `first-last` ranges.
    std::vector<std::string> address_ranges(const std::vector<std::string>& addresses)
    {
        std::vector<std::string> output;
        std::size_t start = 0;
        for (std::size_t index = 0; index < addresses.size(); ++index)
        {
            const auto current = ipv4_value(addresses[index]);
            const auto next =
                index + 1 < addresses.size() ? ipv4_value(addresses[index + 1]) : std::nullopt;
            if (current && next && *next == *current + 1)
            {
                continue;
            }
            output.push_back(
                start == index ? addresses[index] : addresses[start] + '-' + addresses[index]
            );
            start = index + 1;
        }
        return output;
    }

    void print_addresses(const std::vector<std::string>& addresses)
    {
        std::size_t column = 0;
        for (const auto& range : address_ranges(addresses))
        {
            if (column != 0 && column + range.size() + 2 > 78)
            {
                std::cerr << ",\n";
                column = 0;
            }
            std::cerr << (column == 0 ? "  " : ", ") << range;
            column += range.size() + 2;
        }
        std::cerr << '\n';
    }

    /// Report addresses the time limit kept from being fully probed.
    void coverage_warnings(const wlonvif::DiscoveryReport& report, long limit_seconds)
    {
        if (report.unchecked.empty() && report.incomplete.empty())
        {
            return;
        }
        const auto style = style_for(stderr);
        const auto warning = [&]
        {
            std::cerr << '\n' << style(Bold) << style(Yellow) << "Warning:" << style(Reset) << ' ';
        };
        if (!report.unchecked.empty())
        {
            warning();
            std::cerr << report.unchecked.size() << " address(es) were not checked before the "
                      << limit_seconds << "s time limit:\n";
            print_addresses(report.unchecked);
        }
        if (!report.incomplete.empty())
        {
            warning();
            std::cerr << report.incomplete.size()
                      << " address(es) were only partly checked before the time limit,\n"
                         "so a device there may have been missed:\n";
            print_addresses(report.incomplete);
        }
        std::cerr << "Increase --max-time to check them.\n";
    }

    void usage(const char* executable)
    {
        std::cout
            << "Usage: " << executable << " [options]\n\n"
            << "Discovery methods:\n"
            << "  --discover-all         --scan auto with more simultaneous probes\n"
            << "  --target ADDRESS       Unicast-discover and directly inspect one IPv4 address\n"
            << "  --ip ADDRESS           Alias for --target\n"
            << "  --scan CIDR|auto       Directly inspect every address of an explicit or\n"
            << "                         local IPv4 network\n"
            << "  --thorough             Verify discovery results through the Device service\n"
            << "  --interface ADDRESS    Use one IPv4 address for multicast (repeatable)\n"
            << "  --no-multicast         Disable multicast WS-Discovery\n\n"
            << "Limits and output:\n"
            << "  --timeout SECONDS      Time allowed to probe each address (default: 2)\n"
            << "  --max-time SECONDS     Overall limit (default: long enough to probe every\n"
            << "                         address even if each takes the full --timeout)\n"
            << "  --max-hosts COUNT      Maximum expanded scan addresses (default: 1024)\n"
            << "  --jobs COUNT           Simultaneous probes (default: 16; discover-all: 32)\n"
            << "  --json                 Emit machine-readable JSON\n"
            << "  --no-progress          Disable progress output on stderr\n"
            << "  --help                 Show this help\n\n"
            << "Automatic scans cover directly connected networks that fit --max-hosts.\n"
            << "Addresses the time limit prevents from being fully checked are listed,\n"
            << "and the exit status is then 3.\n"
            << "Scan only networks you are authorized to inspect.\n";
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
    const auto& arguments = parsed.value().remaining;
    wlonvif::NetworkDiscoveryOptions options;
    bool auto_networks = false;
    bool discover_all = false;
    bool jobs_set = false;
    std::optional<std::size_t> max_time_seconds;

    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::string_view argument(arguments[index]);
        if (argument == "--help" || argument == "-h")
        {
            usage(argv[0]);
            return 0;
        }
        if (argument == "--discover-all")
        {
            discover_all = true;
            auto_networks = true;
            options.direct_probe = true;
        }
        else if (argument == "--no-multicast")
        {
            options.multicast = false;
        }
        else if (argument == "--thorough")
        {
            options.direct_probe = true;
        }
        else if ((argument == "--target" || argument == "--ip" || argument == "--interface" ||
                  argument == "--scan" || argument == "--max-hosts" || argument == "--jobs" ||
                  argument == "--max-time") &&
                 index + 1 >= arguments.size())
        {
            std::cerr << "Missing value for " << argument << '\n';
            return 2;
        }
        else if (argument == "--target" || argument == "--ip")
        {
            options.targets.push_back(arguments[++index]);
            options.direct_probe = true;
        }
        else if (argument == "--interface")
        {
            options.interfaces.push_back(arguments[++index]);
        }
        else if (argument == "--scan")
        {
            const auto& value = arguments[++index];
            auto_networks = value == "auto";
            if (!auto_networks)
            {
                options.scan_networks.push_back(value);
            }
            options.direct_probe = true;
        }
        else if (argument == "--max-hosts")
        {
            if (!wlonvif::examples::parse_size(arguments[++index], options.maximum_hosts))
            {
                std::cerr << "Invalid maximum host count.\n";
                return 2;
            }
        }
        else if (argument == "--max-time")
        {
            std::size_t seconds = 0;
            if (!wlonvif::examples::parse_size(arguments[++index], seconds) || seconds == 0 ||
                seconds > 86400)
            {
                std::cerr << "Maximum time must be between 1 and 86400 seconds.\n";
                return 2;
            }
            max_time_seconds = seconds;
        }
        else if (argument == "--jobs")
        {
            if (!wlonvif::examples::parse_size(arguments[++index], options.maximum_concurrency) ||
                options.maximum_concurrency > 256)
            {
                std::cerr << "Job count must be between 1 and 256.\n";
                return 2;
            }
            jobs_set = true;
        }
        else
        {
            std::cerr << "Unknown argument: " << argument << '\n';
            usage(argv[0]);
            return 2;
        }
    }

    // --timeout is per address here, not for the whole search.
    if (common.timeout_set)
    {
        options.request_timeout = std::chrono::seconds(common.timeout_seconds);
        options.connect_timeout = std::min(options.connect_timeout, options.request_timeout);
    }
    if (discover_all && !jobs_set)
    {
        options.maximum_concurrency = 32;
    }

    if (auto_networks)
    {
        auto networks = wlonvif::local_ipv4_networks(options.maximum_hosts);
        if (!networks || networks.value().empty())
        {
            std::cerr << "No eligible local IPv4 network fits the host limit.\n";
            return 1;
        }
        options.scan_networks = std::move(networks.value());
        if (!common.json)
        {
            std::cout << "Scanning local network(s):";
            for (const auto& network : options.scan_networks)
            {
                std::cout << ' ' << network;
            }
            std::cout << '\n';
        }
    }

    auto runtime_result = wlonvif::Runtime::create();
    if (!runtime_result)
    {
        std::cerr << "Unable to create runtime: " << runtime_result.error().message << '\n';
        return 1;
    }
    auto runtime = runtime_result.value();
    bool progress_displayed = false;
    const auto started = std::chrono::steady_clock::now();
    const bool terminal = wlonvif::examples::progress_is_terminal();
    if (common.show_progress)
    {
        options.progress =
            [started, terminal, &progress_displayed](const wlonvif::DiscoveryProgress& progress)
        {
            if (!terminal && progress.completed != progress.total && progress.completed % 10 != 0)
            {
                return;
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - started
            );
            const auto label = progress.stage == wlonvif::DiscoveryProgressStage::ws_discovery
                                   ? "WS-Discovery interfaces"
                                   : "Direct probe addresses";
            std::cerr << (terminal ? "\r" : "") << label << ": " << progress.completed << '/'
                      << progress.total;
            if (!progress.current.empty())
            {
                std::cerr << "  " << progress.current;
            }
            std::cerr << "  elapsed " << elapsed.count() << 's';
            std::cerr << (terminal ? "\033[K" : "\n") << std::flush;
            progress_displayed = true;
        };
    }

    const SearchScope search{
        options.multicast,
        !options.scan_networks.empty(),
        static_cast<long>(
            std::chrono::duration_cast<std::chrono::seconds>(options.request_timeout).count()
        )
    };
    std::chrono::milliseconds limit{};
    if (max_time_seconds)
    {
        limit = std::chrono::seconds(*max_time_seconds);
    }
    else
    {
        auto bound = wlonvif::discovery_time_bound(options);
        if (!bound)
        {
            std::cerr << bound.error().message << '\n';
            return 2;
        }
        limit = bound.value();
    }
    const auto limit_seconds =
        static_cast<long>(std::chrono::ceil<std::chrono::seconds>(limit).count());
    auto discovered = wlonvif::examples::wait_for_task(
        wlonvif::discover_network(
            runtime,
            // Devices often advertise their Device service on a non-default port
            // (e.g. 2020); same-host still rejects XAddrs naming another host.
            wlonvif::make_same_host_policy(),
            std::move(options),
            wlonvif::CallOptions{std::chrono::steady_clock::now() + limit}
        )
    );
    if (progress_displayed && terminal)
    {
        // Results replace the progress line rather than following it.
        std::cerr << "\r\033[K" << std::flush;
    }
    (void)runtime->shutdown();
    if (!discovered)
    {
        std::cerr << "Discovery failed: " << discovered.error().message << '\n';
        return 1;
    }
    const auto& report = discovered.value();
    const auto& devices = report.devices;
    // A partial scan is a distinct outcome: an empty or short result is not
    // proof of absence for the addresses that were skipped.
    const int status = report.unchecked.empty() && report.incomplete.empty() ? 0 : 3;

    if (common.json)
    {
        std::cout << '[';
        for (std::size_t index = 0; index < devices.size(); ++index)
        {
            const auto& candidate = devices[index];
            std::cout << (index ? "," : "") << "{\"address\":\""
                      << wlonvif::examples::json_escape(candidate.address) << "\",\"evidence\":\""
                      << evidence_name(candidate.evidence) << '"';
            if (candidate.device_service)
            {
                std::cout << ",\"device_service\":\""
                          << wlonvif::examples::json_escape(*candidate.device_service) << '"';
            }
            std::cout << ",\"diagnostic\":\""
                      << wlonvif::examples::json_escape(candidate.diagnostic) << "\"}";
        }
        std::cout << "]\n";
        coverage_warnings(report, limit_seconds);
        return status;
    }

    const auto style = style_for(stdout);
    if (devices.empty())
    {
        std::cout << "No ONVIF devices found.\n";
    }
    else
    {
        std::cout << '\n'
                  << style(Bold) << "Found " << devices.size() << " ONVIF device"
                  << (devices.size() == 1 ? "" : "s") << style(Reset) << "\n\n";
        for (std::size_t index = 0; index < devices.size(); ++index)
        {
            std::cout << (index ? "\n" : "");
            print_device(devices[index], style);
        }
    }
    std::cout << std::flush;
    coverage_warnings(report, limit_seconds);
    if (devices.empty())
    {
        empty_result_hints(argv[0], search);
    }
    return status;
}

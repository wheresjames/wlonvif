/**
 * @file main.cpp
 * @brief Robust command-line ONVIF discovery and direct-detection example.
 */

#include <chrono>
#include <example_support.hpp>
#include <iostream>
#include <string>
#include <string_view>
#include <wlonvif/wlonvif.hpp>

namespace
{
    std::string_view evidence_name(wlonvif::DeviceEvidence evidence)
    {
        using enum wlonvif::DeviceEvidence;
        switch (evidence)
        {
            case discovery: return "discovered";
            case authentication_challenge: return "authentication-required";
            case soap_fault: return "probable-onvif";
            case onvif_response: return "confirmed-onvif";
        }
        return "unknown";
    }

    void usage(const char* executable)
    {
        std::cout
            << "Usage: " << executable << " [options]\n\n"
            << "Discovery methods:\n"
            << "  --discover-all         Multicast and directly inspect eligible local networks\n"
            << "  --target ADDRESS       Unicast-discover and directly inspect one IPv4 address\n"
            << "  --ip ADDRESS           Alias for --target\n"
            << "  --scan CIDR|auto       Directly inspect an explicit or local IPv4 network\n"
            << "  --thorough             Verify discovery results through the Device service\n"
            << "  --interface ADDRESS    Use one IPv4 address for multicast (repeatable)\n"
            << "  --no-multicast         Disable multicast WS-Discovery\n\n"
            << "Limits and output:\n"
            << "  --timeout SECONDS      Overall deadline (default: 8; discover-all: 60)\n"
            << "  --max-hosts COUNT      Maximum expanded scan addresses (default: 1024)\n"
            << "  --jobs COUNT           Simultaneous probes (default: 16; discover-all: 32)\n"
            << "  --json                 Emit machine-readable JSON\n"
            << "  --no-progress          Disable progress output on stderr\n"
            << "  --help                 Show this help\n\n"
            << "--discover-all scans directly connected networks that fit --max-hosts.\n"
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
        else if (
            (argument == "--target" || argument == "--ip" || argument == "--interface" || argument == "--scan" ||
             argument == "--max-hosts" || argument == "--jobs") &&
            index + 1 >= arguments.size()
        )
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
        else if (argument == "--jobs")
        {
            if (!wlonvif::examples::parse_size(
                    arguments[++index], options.maximum_concurrency
                ) ||
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

    if (!common.timeout_set)
    {
        common.timeout_seconds = discover_all ? 60 : 8;
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
            std::cout << (discover_all ? "Discover-all direct-scan coverage:"
                                      : "Scanning authorized local network range(s):");
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
        options.progress = [started, terminal, &progress_displayed](
                               const wlonvif::DiscoveryProgress& progress
                           )
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

    auto discovered = wlonvif::examples::wait_for_task(wlonvif::discover_devices(
        runtime,
        wlonvif::make_same_origin_policy(),
        std::move(options),
        wlonvif::examples::call_options(common.timeout_seconds)
    ));
    if (progress_displayed && terminal)
    {
        std::cerr << '\n';
    }
    (void)runtime->shutdown();
    if (!discovered)
    {
        std::cerr << "Discovery failed: " << discovered.error().message << '\n';
        return 1;
    }

    if (common.json)
    {
        std::cout << '[';
        for (std::size_t index = 0; index < discovered.value().size(); ++index)
        {
            const auto& candidate = discovered.value()[index];
            std::cout << (index ? "," : "") << "{\"address\":\""
                      << wlonvif::examples::json_escape(candidate.address)
                      << "\",\"evidence\":\"" << evidence_name(candidate.evidence) << '"';
            if (candidate.device_service)
            {
                std::cout << ",\"device_service\":\""
                          << wlonvif::examples::json_escape(*candidate.device_service) << '"';
            }
            std::cout << ",\"diagnostic\":\""
                      << wlonvif::examples::json_escape(candidate.diagnostic) << "\"}";
        }
        std::cout << "]\n";
        return 0;
    }

    for (const auto& candidate : discovered.value())
    {
        std::cout << candidate.address << "  [" << evidence_name(candidate.evidence) << "]\n";
        if (candidate.device_service)
        {
            std::cout << "  Device service: " << *candidate.device_service << '\n';
        }
        if (candidate.discovery)
        {
            std::cout << "  Endpoint: " << candidate.discovery->endpoint_reference << '\n';
            for (const auto& scope : candidate.discovery->scopes)
            {
                std::cout << "  Scope: " << scope << '\n';
            }
        }
        std::cout << "  Evidence: " << candidate.diagnostic << '\n';
    }
    std::cout << "Found " << discovered.value().size() << " unique ONVIF candidate(s).\n";
    return 0;
}

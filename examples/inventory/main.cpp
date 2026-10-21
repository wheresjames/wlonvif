/**
 * @file main.cpp
 * @brief wlonvif-inventory: find the cameras on a network and tabulate what each is.
 *
 *     wlonvif-inventory --scan auto --username admin --media --ptz
 *
 * Two phases: discovery (WS-Discovery multicast plus, with --scan or --target,
 * direct probes of each address), then a pool of --jobs workers that connect to
 * every camera found and read its identity, plus Media, PTZ, and Events details
 * when asked. One row per camera; a camera that fails gets its error in its row.
 */

#include <atomic>
#include <example_support.hpp>
#include <iostream>
#include <mutex>
#include <thread>

namespace
{
    struct InventoryItem
    {
        std::string address;
        std::string device_service;
        std::optional<wlonvif::DeviceInformation> information;
        std::optional<std::size_t> media_profiles;
        std::optional<bool> ptz_advertised;
        std::optional<bool> events_advertised;
        std::optional<wlonvif::Error> capability_error;
        std::optional<wlonvif::Error> error;
    };

    void usage(const char* executable)
    {
        std::cout
            << "Usage: " << executable << " [discovery options] [common options]\n\n"
            << "  --target ADDRESS       Inspect an explicit address (repeatable)\n"
            << "  --ip ADDRESS           Alias for --target\n"
            << "  --scan CIDR|auto       Inspect an authorized network\n"
            << "  --jobs COUNT           Concurrent Device queries (default: 8)\n"
            << "  --no-multicast         Disable multicast discovery\n"
            << "  --media --ptz --events Add optional service capability columns\n"
            << "  --timeout SECONDS --username NAME --password PASSWORD --password-file FILE\n"
            << "  --password-env NAME --json --no-progress --help\n";
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
    wlonvif::NetworkDiscoveryOptions discovery;
    discovery.direct_probe = true;
    std::size_t jobs = 8;
    bool auto_networks = false;
    bool inspect_media = false, inspect_ptz = false, inspect_events = false;
    for (std::size_t index = 0; index < parsed.value().remaining.size(); ++index)
    {
        const auto& argument = parsed.value().remaining[index];
        if (argument == "--help" || argument == "-h")
        {
            usage(argv[0]);
            return 0;
        }
        if ((argument == "--target" || argument == "--ip" || argument == "--scan" ||
             argument == "--jobs") &&
            index + 1 >= parsed.value().remaining.size())
        {
            std::cerr << "Missing value for " << argument << '\n';
            return 2;
        }
        if (argument == "--target" || argument == "--ip")
        {
            discovery.targets.push_back(parsed.value().remaining[++index]);
        }
        else if (argument == "--scan")
        {
            const auto& value = parsed.value().remaining[++index];
            auto_networks = value == "auto";
            if (!auto_networks)
            {
                discovery.scan_networks.push_back(value);
            }
        }
        else if (argument == "--jobs")
        {
            if (!wlonvif::examples::parse_size(parsed.value().remaining[++index], jobs) ||
                jobs > 64)
            {
                std::cerr << "Job count must be between 1 and 64\n";
                return 2;
            }
        }
        else if (argument == "--no-multicast")
        {
            discovery.multicast = false;
        }
        else if (argument == "--media")
        {
            inspect_media = true;
        }
        else if (argument == "--ptz")
        {
            inspect_ptz = true;
        }
        else if (argument == "--events")
        {
            inspect_events = true;
        }
        else
        {
            std::cerr << "Unknown argument: " << argument << '\n';
            return 2;
        }
    }
    if (auto_networks)
    {
        auto networks = wlonvif::local_ipv4_networks(discovery.maximum_hosts);
        if (!networks || networks.value().empty())
        {
            std::cerr << "No eligible local IPv4 networks\n";
            return 1;
        }
        discovery.scan_networks = std::move(networks.value());
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
    const bool terminal_progress = wlonvif::examples::progress_is_terminal();
    if (common.show_progress)
    {
        discovery.progress = [terminal_progress](const wlonvif::DiscoveryProgress& progress)
        {
            if (!terminal_progress && progress.completed != progress.total &&
                progress.completed % 10 != 0)
            {
                return;
            }
            std::cerr << (terminal_progress ? "\r" : "") << "Discovery " << progress.completed
                      << '/' << progress.total << "  " << progress.current
                      << (terminal_progress ? "\033[K" : "\n") << std::flush;
        };
    }
    // ---- Phase 1: discovery ----------------------------------------------------------
    auto found = wlonvif::examples::wait_for_task(
        wlonvif::discover_devices(
            runtime,
            wlonvif::make_same_origin_policy(),
            std::move(discovery),
            wlonvif::examples::call_options(common.timeout_seconds)
        )
    );
    if (common.show_progress && terminal_progress)
    {
        std::cerr << '\n';
    }
    if (!found)
    {
        std::cerr << "Discovery failed: " << found.error().message << '\n';
        (void)runtime->shutdown();
        return 1;
    }

    // ---- Phase 2: query each camera, --jobs at a time ------------------------------------
    // One preallocated row per camera, written only by the worker that took it.
    std::vector<InventoryItem> items(found.value().size());
    std::atomic_size_t next{};
    std::atomic_size_t completed{};
    std::mutex progress_mutex;
    std::vector<std::jthread> workers;
    const auto worker_count = std::min(jobs, found.value().size());
    workers.reserve(worker_count);
    for (std::size_t worker = 0; worker < worker_count; ++worker)
    {
        workers.emplace_back(
            [&]
            {
                for (;;)
                {
                    const auto index = next.fetch_add(1);
                    if (index >= found.value().size())
                    {
                        return;
                    }
                    const auto& candidate = found.value()[index];
                    auto& item = items[index];
                    item.address = candidate.address;
                    if (!candidate.device_service)
                    {
                        item.error = wlonvif::Error{
                            wlonvif::ErrorCategory::unsupported,
                            {},
                            "no Device service URL was discovered"
                        };
                    }
                    else
                    {
                        item.device_service = *candidate.device_service;
                        auto session = wlonvif::examples::connect_device(
                            item.device_service,
                            runtime,
                            credentials.value(),
                            common.timeout_seconds
                        );
                        if (!session)
                        {
                            item.error = session.error();
                        }
                        else
                        {
                            auto information = wlonvif::examples::wait_for_task(
                                session.value()->get_device_information(
                                    wlonvif::examples::call_options(common.timeout_seconds)
                                )
                            );
                            if (information)
                            {
                                item.information = std::move(information.value());
                            }
                            else
                            {
                                item.error = information.error();
                            }
                            if (!item.error && (inspect_media || inspect_ptz || inspect_events))
                            {
                                auto capabilities = wlonvif::examples::wait_for_task(
                                    session.value()->get_capabilities(
                                        wlonvif::examples::call_options(common.timeout_seconds)
                                    )
                                );
                                if (!capabilities)
                                {
                                    item.capability_error = capabilities.error();
                                }
                                else
                                {
                                    if (inspect_ptz)
                                    {
                                        item.ptz_advertised =
                                            capabilities.value().ptz_xaddr.has_value();
                                    }
                                    if (inspect_events)
                                    {
                                        item.events_advertised =
                                            capabilities.value().events_xaddr.has_value();
                                    }
                                }
                                if (inspect_media)
                                {
                                    auto media = session.value()->media();
                                    if (!media)
                                    {
                                        item.capability_error = media.error();
                                    }
                                    else
                                    {
                                        auto profiles = wlonvif::examples::wait_for_task(
                                            media.value()->get_profiles(
                                                wlonvif::examples::call_options(
                                                    common.timeout_seconds
                                                )
                                            )
                                        );
                                        if (profiles)
                                        {
                                            item.media_profiles = profiles.value().size();
                                        }
                                        else
                                        {
                                            item.capability_error = profiles.error();
                                        }
                                    }
                                }
                            }
                        }
                    }
                    const auto count = completed.fetch_add(1) + 1;
                    if (common.show_progress)
                    {
                        std::lock_guard lock(progress_mutex);
                        if (terminal_progress || count == items.size() || count % 10 == 0)
                        {
                            std::cerr << (terminal_progress ? "\r" : "") << "Device queries "
                                      << count << '/' << items.size() << "  " << item.address
                                      << (terminal_progress ? "\033[K" : "\n") << std::flush;
                        }
                    }
                }
            }
        );
    }
    workers.clear();
    if (common.show_progress && terminal_progress && !items.empty())
    {
        std::cerr << '\n';
    }

    if (common.json)
    {
        std::cout << '[';
    }
    std::size_t failures = 0;
    for (std::size_t index = 0; index < items.size(); ++index)
    {
        const auto& item = items[index];
        failures += item.error.has_value();
        if (common.json)
        {
            std::cout << (index ? "," : "") << "{\"address\":\""
                      << wlonvif::examples::json_escape(item.address) << "\",\"device_service\":\""
                      << wlonvif::examples::json_escape(
                             wlonvif::examples::redact_url(item.device_service)
                         )
                      << '"';
            if (item.information)
            {
                std::cout << ",\"manufacturer\":\""
                          << wlonvif::examples::json_escape(item.information->manufacturer)
                          << "\",\"model\":\""
                          << wlonvif::examples::json_escape(item.information->model) << '"';
            }
            if (item.error)
            {
                std::cout << ",\"error\":\""
                          << wlonvif::examples::error_category_name(item.error->category) << '"';
            }
            if (item.media_profiles)
            {
                std::cout << ",\"media_profiles\":" << *item.media_profiles;
            }
            if (item.ptz_advertised)
            {
                std::cout << ",\"ptz\":" << (*item.ptz_advertised ? "true" : "false");
            }
            if (item.events_advertised)
            {
                std::cout << ",\"events\":" << (*item.events_advertised ? "true" : "false");
            }
            if (item.capability_error)
            {
                std::cout << ",\"capability_error\":\""
                          << wlonvif::examples::error_category_name(item.capability_error->category)
                          << '"';
            }
            std::cout << '}';
        }
        else
        {
            std::cout << item.address << "  ";
            if (item.information)
            {
                std::cout << item.information->manufacturer << ' ' << item.information->model;
            }
            else
            {
                std::cout << '[' << wlonvif::examples::error_category_name(item.error->category)
                          << "] " << item.error->message;
            }
            if (item.media_profiles)
            {
                std::cout << "  media_profiles=" << *item.media_profiles;
            }
            if (item.ptz_advertised)
            {
                std::cout << "  ptz=" << (*item.ptz_advertised ? "yes" : "no");
            }
            if (item.events_advertised)
            {
                std::cout << "  events=" << (*item.events_advertised ? "yes" : "no");
            }
            if (item.capability_error)
            {
                std::cout << "  capability="
                          << wlonvif::examples::error_category_name(
                                 item.capability_error->category
                             );
            }
            std::cout << '\n';
        }
    }
    if (common.json)
    {
        std::cout << "]\n";
    }
    (void)runtime->shutdown();
    return failures == items.size() && !items.empty() ? 1 : 0;
}

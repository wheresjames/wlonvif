/** @file main.cpp @brief Inspect multiple cameras with bounded concurrency. */

#include <atomic>
#include <example_support.hpp>
#include <iostream>
#include <mutex>
#include <thread>

namespace
{
    struct DeviceReference
    {
        std::string url;
        std::string address;
    };

    struct ResultRow
    {
        std::string url;
        std::string identity;
        std::size_t profiles{};
        std::optional<wlonvif::Error> error;
    };
}

int main(int argc, char** argv)
{
    auto parsed = wlonvif::examples::parse_common_arguments(argc, argv);
    if (!parsed) { std::cerr << parsed.error().message << '\n'; return 2; }
    auto common = std::move(parsed.value().common);
    std::vector<DeviceReference> devices;
    std::size_t jobs = 4;
    for (std::size_t index = 0; index < parsed.value().remaining.size(); ++index)
    {
        const auto& argument = parsed.value().remaining[index];
        if (argument == "--help" || argument == "-h")
        {
            std::cout << "Usage: " << argv[0]
                      << " (--url DEVICE_URL | --ip ADDRESS) [...] [--jobs 1..32]"
                         " [common options]\n"
                      << "Credentials: --username NAME --password PASSWORD; environment and"
                         " prompt fallbacks apply.\n";
            return 0;
        }
        if ((argument == "--url" || argument == "--ip" || argument == "--jobs") &&
            index + 1 >= parsed.value().remaining.size())
        {
            std::cerr << "Missing value for " << argument << '\n';
            return 2;
        }
        if (argument == "--url")
            devices.push_back({parsed.value().remaining[++index], {}});
        else if (argument == "--ip")
            devices.push_back({{}, parsed.value().remaining[++index]});
        else if (argument == "--jobs")
        {
            if (!wlonvif::examples::parse_size(parsed.value().remaining[++index], jobs) || jobs > 32)
            {
                std::cerr << "Job count must be between 1 and 32\n";
                return 2;
            }
        }
        else { std::cerr << "Unknown argument: " << argument << '\n'; return 2; }
    }
    if (devices.empty())
    {
        std::cerr << "At least one --url or --ip is required\n";
        return 2;
    }
    auto credentials = wlonvif::examples::load_credentials(common);
    if (!credentials) { std::cerr << credentials.error().message << '\n'; return 2; }
    auto created = wlonvif::Runtime::create();
    if (!created) { std::cerr << created.error().message << '\n'; return 1; }
    auto runtime = created.value();
    std::vector<ResultRow> rows(devices.size());
    std::atomic_size_t next{};
    std::atomic_size_t complete{};
    std::mutex progress_mutex;
    const bool terminal = wlonvif::examples::progress_is_terminal();
    std::vector<std::jthread> workers;
    for (std::size_t worker = 0; worker < std::min(jobs, devices.size()); ++worker)
    {
        workers.emplace_back([&]
        {
            for (;;)
            {
                const auto index = next.fetch_add(1);
                if (index >= devices.size()) return;
                auto& row = rows[index];
                const auto& reference = devices[index];
                row.url = reference.url.empty() ? reference.address : reference.url;
                auto endpoint = wlonvif::examples::resolve_device_endpoint(
                    reference.url, reference.address, runtime, common.timeout_seconds
                );
                if (!endpoint) row.error = endpoint.error();
                else
                {
                    row.url = std::move(endpoint.value().url);
                    auto session = wlonvif::examples::connect_device(
                        row.url, runtime, credentials.value(), common.timeout_seconds
                    );
                    if (!session)
                    {
                        row.error = session.error();
                    }
                    else
                    {
                        auto information = wlonvif::examples::wait_for_task(
                            session.value()->get_device_information(
                                wlonvif::examples::call_options(common.timeout_seconds)
                            )
                        );
                        if (!information) row.error = information.error();
                        else
                        {
                            row.identity = information.value().manufacturer + " " +
                                           information.value().model;
                            auto media = session.value()->media();
                            if (media)
                            {
                                auto profiles = wlonvif::examples::wait_for_task(
                                    media.value()->get_profiles(
                                        wlonvif::examples::call_options(common.timeout_seconds)
                                    )
                                );
                                if (profiles) row.profiles = profiles.value().size();
                                else row.error = profiles.error();
                            }
                            else row.error = media.error();
                        }
                    }
                }
                const auto done = complete.fetch_add(1) + 1;
                if (common.show_progress)
                {
                    std::lock_guard lock(progress_mutex);
                    std::cerr << (terminal ? "\r" : "") << "Camera inspection " << done << '/'
                              << rows.size() << "  " << wlonvif::examples::redact_url(row.url)
                              << (terminal ? "\033[K" : "\n") << std::flush;
                }
            }
        });
    }
    workers.clear();
    if (common.show_progress && terminal) std::cerr << '\n';
    std::size_t failures = 0;
    if (common.json) std::cout << '[';
    for (std::size_t index = 0; index < rows.size(); ++index)
    {
        const auto& row = rows[index];
        failures += row.error.has_value();
        if (common.json)
        {
            std::cout << (index ? "," : "") << "{\"url\":\""
                      << wlonvif::examples::json_escape(wlonvif::examples::redact_url(row.url))
                      << "\",\"identity\":\"" << wlonvif::examples::json_escape(row.identity)
                      << "\",\"profiles\":" << row.profiles;
            if (row.error) std::cout << ",\"error\":\""
                                     << wlonvif::examples::error_category_name(row.error->category)
                                     << '"';
            std::cout << '}';
        }
        else
        {
            std::cout << wlonvif::examples::redact_url(row.url) << "  ";
            if (row.error) std::cout << '[' << wlonvif::examples::error_category_name(row.error->category)
                                     << "] " << row.error->message;
            else std::cout << row.identity << "  profiles=" << row.profiles;
            std::cout << '\n';
        }
    }
    if (common.json) std::cout << "]\n";
    (void)runtime->shutdown();
    return failures == rows.size() ? 1 : 0;
}

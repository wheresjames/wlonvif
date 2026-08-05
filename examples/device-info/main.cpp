/** @file main.cpp @brief Query read-only information from a real ONVIF Device service. */

#include <chrono>
#include <ctime>
#include <example_support.hpp>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace
{
    std::string utc_text(std::chrono::system_clock::time_point value)
    {
        const auto time = std::chrono::system_clock::to_time_t(value);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &time);
#else
        gmtime_r(&time, &utc);
#endif
        std::ostringstream output;
        output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
        return output.str();
    }

    void usage(const char* executable)
    {
        std::cout
            << "Usage: " << executable << " (--url URL | --ip ADDRESS) [sections] [common options]\n\n"
            << "Sections (default: --all):\n"
            << "  --identity --clock --scopes --services --capabilities --all\n\n"
            << "Common options:\n"
            << "  --timeout SECONDS --username NAME --password PASSWORD --password-file FILE\n"
            << "  --password-env NAME --allow-cleartext-basic --json --help\n";
    }
}

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
    bool identity = false, clock = false, scopes = false, services = false, capabilities = false;
    for (std::size_t index = 0; index < parsed.value().remaining.size(); ++index)
    {
        const auto& argument = parsed.value().remaining[index];
        if (argument == "--help" || argument == "-h")
        {
            usage(argv[0]);
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
        else if (argument == "--identity") identity = true;
        else if (argument == "--clock") clock = true;
        else if (argument == "--scopes") scopes = true;
        else if (argument == "--services") services = true;
        else if (argument == "--capabilities") capabilities = true;
        else if (argument == "--all") identity = clock = scopes = services = capabilities = true;
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
    if (!identity && !clock && !scopes && !services && !capabilities)
    {
        identity = clock = scopes = services = capabilities = true;
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
    auto endpoint = wlonvif::examples::resolve_device_endpoint(
        url, address, runtime, common.timeout_seconds
    );
    if (!endpoint)
    {
        std::cerr << "Endpoint discovery failed: " << endpoint.error().message << '\n';
        (void)runtime->shutdown();
        return 1;
    }
    url = std::move(endpoint.value().url);
    auto connected = wlonvif::examples::connect_device(
        url, runtime, std::move(credentials.value()), common.timeout_seconds
    );
    if (!connected)
    {
        std::cerr << "Connection failed ["
                  << wlonvif::examples::error_category_name(connected.error().category)
                  << "]: " << connected.error().message << '\n';
        (void)runtime->shutdown();
        return 1;
    }
    auto device = connected.value();
    bool first = true;
    if (common.json) std::cout << '{';
    const auto separator = [&]
    {
        if (!first) std::cout << ',';
        first = false;
    };
    const auto fail = [&](const wlonvif::Error& error)
    {
        std::cerr << "Query failed [" << wlonvif::examples::error_category_name(error.category)
                  << "]: " << error.message << '\n';
        return 1;
    };

    if (identity)
    {
        auto result = wlonvif::examples::wait_for_task(
            device->get_device_information(wlonvif::examples::call_options(common.timeout_seconds))
        );
        if (!result) return fail(result.error());
        const auto& value = result.value();
        if (common.json)
        {
            separator();
            std::cout << "\"identity\":{\"manufacturer\":\""
                      << wlonvif::examples::json_escape(value.manufacturer) << "\",\"model\":\""
                      << wlonvif::examples::json_escape(value.model) << "\",\"firmware\":\""
                      << wlonvif::examples::json_escape(value.firmware_version)
                      << "\",\"serial_number\":\""
                      << wlonvif::examples::json_escape(value.serial_number)
                      << "\",\"hardware_id\":\""
                      << wlonvif::examples::json_escape(value.hardware_id) << "\"}";
        }
        else
        {
            std::cout << "Manufacturer: " << value.manufacturer << "\nModel: " << value.model
                      << "\nFirmware: " << value.firmware_version
                      << "\nSerial number: " << value.serial_number
                      << "\nHardware ID: " << value.hardware_id << '\n';
        }
    }
    if (clock)
    {
        auto result = wlonvif::examples::wait_for_task(
            device->get_system_date_and_time(wlonvif::examples::call_options(common.timeout_seconds))
        );
        if (!result) return fail(result.error());
        const auto text = utc_text(result.value().utc);
        if (common.json) { separator(); std::cout << "\"utc\":\"" << text << '"'; }
        else std::cout << "UTC: " << text << '\n';
    }
    if (scopes)
    {
        auto result = wlonvif::examples::wait_for_task(
            device->get_scopes(wlonvif::examples::call_options(common.timeout_seconds))
        );
        if (!result) return fail(result.error());
        if (common.json)
        {
            separator(); std::cout << "\"scopes\":[";
            for (std::size_t i = 0; i < result.value().size(); ++i)
                std::cout << (i ? "," : "") << '"'
                          << wlonvif::examples::json_escape(result.value()[i]) << '"';
            std::cout << ']';
        }
        else for (const auto& value : result.value()) std::cout << "Scope: " << value << '\n';
    }
    if (services)
    {
        auto result = wlonvif::examples::wait_for_task(
            device->get_services(wlonvif::examples::call_options(common.timeout_seconds))
        );
        if (!result) return fail(result.error());
        if (common.json)
        {
            separator(); std::cout << "\"services\":[";
            for (std::size_t i = 0; i < result.value().size(); ++i)
            {
                const auto& value = result.value()[i];
                std::cout << (i ? "," : "") << "{\"namespace\":\""
                          << wlonvif::examples::json_escape(value.namespace_uri)
                          << "\",\"xaddr\":\""
                          << wlonvif::examples::json_escape(wlonvif::examples::redact_url(value.xaddr))
                          << "\"}";
            }
            std::cout << ']';
        }
        else for (const auto& value : result.value())
            std::cout << "Service: " << value.namespace_uri << "\n  "
                      << wlonvif::examples::redact_url(value.xaddr) << '\n';
    }
    if (capabilities)
    {
        auto result = wlonvif::examples::wait_for_task(
            device->get_capabilities(wlonvif::examples::call_options(common.timeout_seconds))
        );
        if (!result) return fail(result.error());
        const auto& value = result.value();
        if (common.json)
        {
            separator(); std::cout << "\"capabilities\":{";
            std::cout << "\"media\":" << (value.media_xaddr ? "\"" + wlonvif::examples::json_escape(wlonvif::examples::redact_url(*value.media_xaddr)) + "\"" : "null") << ',';
            std::cout << "\"ptz\":" << (value.ptz_xaddr ? "\"" + wlonvif::examples::json_escape(wlonvif::examples::redact_url(*value.ptz_xaddr)) + "\"" : "null") << ',';
            std::cout << "\"events\":" << (value.events_xaddr ? "\"" + wlonvif::examples::json_escape(wlonvif::examples::redact_url(*value.events_xaddr)) + "\"" : "null") << '}';
        }
        else
        {
            std::cout << "Media: " << (value.media_xaddr ? wlonvif::examples::redact_url(*value.media_xaddr) : "not advertised") << '\n';
            std::cout << "PTZ: " << (value.ptz_xaddr ? wlonvif::examples::redact_url(*value.ptz_xaddr) : "not advertised") << '\n';
            std::cout << "Events: " << (value.events_xaddr ? wlonvif::examples::redact_url(*value.events_xaddr) : "not advertised") << '\n';
        }
    }
    if (common.json) std::cout << "}\n";
    (void)runtime->shutdown();
    return 0;
}

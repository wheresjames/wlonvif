/**
 * @file main.cpp
 * @brief wlonvif-call: send any ONVIF request and print the response.
 *
 * For operations the library has no typed call for. The body is wrapped in a
 * SOAP envelope, signed with the credentials like any other request, and sent
 * to the endpoint the camera advertises for the chosen service, so the usual
 * endpoint policy and clock correction apply.
 */

#include <algorithm>
#include <example_support.hpp>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace
{
    void usage(const char* executable)
    {
        std::cout
            << "Usage: " << executable
            << " (--url URL | --ip ADDRESS) [--service NAME] [--response NAME] BODY [common "
               "options]\n\n"
            << "Send BODY, the complete SOAP Body content, and print the response element.\n"
            << "The request is signed with the given credentials and sent to the endpoint the\n"
            << "device advertises for the service.\n\n"
            << "  --service NAME   device (default), media, ptz, events, analytics, or imaging\n"
            << "  --response NAME  Expected response element (default: BODY's operation name\n"
            << "                   followed by Response)\n"
            << "  --raw            Print the response markup unindented\n\n"
            << "Declared prefixes: tds trt tptz tev tan timg tt wsnt tns1\n\n"
            << "Examples:\n"
            << "  " << executable << " --ip 192.0.2.10 '<tds:GetZeroConfiguration/>'\n"
            << "  " << executable << " --ip 192.0.2.10 --service media '<trt:GetAudioOutputs/>'\n\n"
            << "Common options:\n"
            << "  --timeout SECONDS --username NAME --password PASSWORD --password-file FILE\n"
            << "  --password-env NAME --allow-cleartext-basic --help\n";
    }

    std::optional<wlonvif::OnvifService> service_named(std::string_view name)
    {
        using enum wlonvif::OnvifService;
        if (name == "device")
        {
            return device;
        }
        if (name == "media")
        {
            return media;
        }
        if (name == "ptz")
        {
            return ptz;
        }
        if (name == "events")
        {
            return events;
        }
        if (name == "analytics")
        {
            return analytics;
        }
        if (name == "imaging")
        {
            return imaging;
        }
        return std::nullopt;
    }

    /// Put each element on its own line, indented by depth; text stays inline.
    std::string indented(std::string_view xml)
    {
        std::string output;
        int depth = 0;
        bool text_since_open = false;
        for (std::size_t index = 0; index < xml.size();)
        {
            if (xml[index] != '<')
            {
                const auto next = xml.find('<', index);
                const auto text = xml.substr(index, next - index);
                if (text.find_first_not_of(" \t\r\n") != std::string_view::npos)
                {
                    output += text;
                    text_since_open = true;
                }
                index = next == std::string_view::npos ? xml.size() : next;
                continue;
            }
            const auto end = xml.find('>', index);
            const auto tag = xml.substr(
                index, end == std::string_view::npos ? xml.size() - index : end - index + 1
            );
            index += tag.size();
            const bool closing = tag.starts_with("</");
            const bool self_closing = tag.ends_with("/>");
            if (closing)
            {
                --depth;
            }
            // A closing tag right after its own text stays on the same line.
            if (!(closing && text_since_open))
            {
                if (!output.empty())
                {
                    output += '\n';
                }
                output.append(static_cast<std::size_t>(std::max(depth, 0)) * 2, ' ');
            }
            output += tag;
            text_since_open = false;
            if (!closing && !self_closing && !tag.starts_with("<?") && !tag.starts_with("<!"))
            {
                ++depth;
            }
        }
        return output;
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
    std::string url, address, body, response;
    auto service = wlonvif::OnvifService::device;
    bool raw = false;
    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::string_view argument(arguments[index]);
        if (argument == "--help" || argument == "-h")
        {
            usage(argv[0]);
            return 0;
        }
        const bool needs_value = argument == "--url" || argument == "--ip" ||
                                 argument == "--service" || argument == "--response";
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
        else if (argument == "--response")
        {
            response = arguments[++index];
        }
        else if (argument == "--raw")
        {
            raw = true;
        }
        else if (argument == "--service")
        {
            const auto named = service_named(arguments[++index]);
            if (!named)
            {
                std::cerr << "Unknown service: " << arguments[index] << '\n';
                return 2;
            }
            service = *named;
        }
        else if (body.empty() && argument.starts_with("<"))
        {
            body = arguments[index];
        }
        else
        {
            std::cerr << "Unknown argument: " << argument << '\n';
            usage(argv[0]);
            return 2;
        }
    }
    if (url.empty() == address.empty() || body.empty())
    {
        usage(argv[0]);
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
        std::cerr << "Connection failed: " << connected.error().message << '\n';
        return finish(1);
    }
    auto result = wlonvif::examples::wait_for_task(connected.value()->call(
        service, body, response, wlonvif::examples::call_options(common.timeout_seconds)
    ));
    if (!result)
    {
        const auto& error = result.error();
        std::cerr << "Request failed [" << wlonvif::examples::error_category_name(error.category)
                  << "]: " << error.message << '\n';
        if (error.soap_fault)
        {
            const auto& fault = *error.soap_fault;
            std::cerr << "  code:    " << fault.code << '\n';
            if (!fault.subcode.empty())
            {
                std::cerr << "  subcode: " << fault.subcode << '\n';
            }
            if (!fault.reason.empty())
            {
                std::cerr << "  reason:  " << fault.reason << '\n';
            }
        }
        return finish(1);
    }
    std::cout << (raw ? result.value() : indented(result.value())) << '\n';
    return finish(0);
}

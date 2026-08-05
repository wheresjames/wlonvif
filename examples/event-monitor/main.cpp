/** @file main.cpp @brief Monitor a real ONVIF PullPoint Event subscription. */

#include <example_support.hpp>
#include <iomanip>
#include <iostream>

int main(int argc, char** argv)
{
    auto parsed = wlonvif::examples::parse_common_arguments(argc, argv);
    if (!parsed)
    {
        std::cerr << parsed.error().message << '\n';
        return 2;
    }
    auto common = std::move(parsed.value().common);
    std::string url, address, topic_filter;
    std::size_t count = 0;
    for (std::size_t index = 0; index < parsed.value().remaining.size(); ++index)
    {
        const auto& argument = parsed.value().remaining[index];
        if (argument == "--help" || argument == "-h")
        {
            std::cout << "Usage: " << argv[0]
                      << " (--url DEVICE_URL | --ip ADDRESS) [--topic TEXT] [--count COUNT]"
                         " [common options]\n"
                      << "Credentials: --username NAME --password PASSWORD; environment and"
                         " prompt fallbacks apply.\n";
            return 0;
        }
        if ((argument == "--url" || argument == "--ip" || argument == "--topic" ||
             argument == "--count") &&
            index + 1 >= parsed.value().remaining.size())
        {
            std::cerr << "Missing value for " << argument << '\n';
            return 2;
        }
        if (argument == "--url") url = parsed.value().remaining[++index];
        else if (argument == "--ip") address = parsed.value().remaining[++index];
        else if (argument == "--topic") topic_filter = parsed.value().remaining[++index];
        else if (argument == "--count")
        {
            if (!wlonvif::examples::parse_size(parsed.value().remaining[++index], count))
            {
                std::cerr << "Invalid event count\n";
                return 2;
            }
        }
        else
        {
            std::cerr << "Unknown argument: " << argument << '\n';
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
    auto runtime_result = wlonvif::Runtime::create();
    if (!runtime_result) return 1;
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
    auto session = wlonvif::examples::connect_device(
        url, runtime, credentials.value(), common.timeout_seconds
    );
    if (!session)
    {
        std::cerr << session.error().message << '\n';
        return 1;
    }
    auto stream = session.value()->events(256);
    if (!stream)
    {
        std::cerr << stream.error().message << '\n';
        return 1;
    }
    std::size_t delivered = 0;
    while (count == 0 || delivered < count)
    {
        auto polled = wlonvif::examples::wait_for_task(
            stream.value()->poll(32, wlonvif::examples::call_options(common.timeout_seconds))
        );
        if (!polled)
        {
            if (polled.error().category == wlonvif::ErrorCategory::cancelled) break;
            std::cerr << "Event pull failed: " << polled.error().message << '\n';
            auto marked = wlonvif::examples::wait_for_task(stream.value()->mark_reconnected(
                wlonvif::examples::call_options(common.timeout_seconds)
            ));
            if (!marked) break;
            continue;
        }
        for (;;)
        {
            auto item = wlonvif::examples::wait_for_task(
                stream.value()->next(wlonvif::examples::call_options(1))
            );
            if (!item)
            {
                if (item.error().category == wlonvif::ErrorCategory::timeout) break;
                goto finished;
            }
            if (const auto* health = std::get_if<wlonvif::StreamHealth>(&item.value()))
            {
                std::cerr << "Event stream health: "
                          << (*health == wlonvif::StreamHealth::possible_event_loss
                                  ? "possible event loss"
                                  : "events dropped")
                          << '\n';
                continue;
            }
            const auto& event = std::get<wlonvif::Event>(item.value());
            if (!topic_filter.empty() && event.topic.find(topic_filter) == std::string::npos)
                continue;
            if (common.json)
            {
                std::cout << "{\"topic\":\"" << wlonvif::examples::json_escape(event.topic)
                          << "\",\"source\":\""
                          << wlonvif::examples::json_escape(event.source) << "\",\"data\":\""
                          << wlonvif::examples::json_escape(event.data) << "\",\"sequence\":"
                          << event.sequence << "}\n";
            }
            else
            {
                std::cout << event.sequence << "  " << event.topic << "  " << event.source
                          << "  " << event.data << '\n';
            }
            ++delivered;
            if (count && delivered >= count) goto finished;
        }
    }
finished:
    auto closed = wlonvif::examples::wait_for_task(
        stream.value()->close(wlonvif::examples::safety_call_options(5))
    );
    if (!closed && closed.error().category != wlonvif::ErrorCategory::cancelled)
        std::cerr << "Unsubscribe failed: " << closed.error().message << '\n';
    (void)runtime->shutdown();
    return 0;
}

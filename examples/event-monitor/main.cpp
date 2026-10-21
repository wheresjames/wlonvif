/**
 * @file main.cpp
 * @brief wlonvif-event-monitor: print a camera's events (motion, tamper, ...) as they happen.
 *
 *     wlonvif-event-monitor --ip 192.0.2.10 --topics
 *     wlonvif-event-monitor --ip 192.0.2.10 --subscribe tns1:RuleEngine/CellMotionDetector/Motion
 *
 * Uses a managed event stream: the library creates a PullPoint subscription,
 * keeps pulling and renewing it, and re-creates it if the camera drops it,
 * reporting a possible gap as a health signal. --subscribe asks the camera to
 * send only some topics; --topic filters on this side instead. --count stops
 * after that many events; otherwise Ctrl-C stops.
 */

#include <example_support.hpp>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

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
    std::vector<std::string> subscribe;
    bool list_topics = false;
    std::size_t count = 0;
    for (std::size_t index = 0; index < parsed.value().remaining.size(); ++index)
    {
        const auto& argument = parsed.value().remaining[index];
        if (argument == "--help" || argument == "-h")
        {
            std::cout << "Usage: " << argv[0]
                      << " (--url DEVICE_URL | --ip ADDRESS) [--topic TEXT] [--count COUNT]"
                         " [common options]\n"
                      << "  --topics           List the topics the device can emit and exit\n"
                      << "  --subscribe TOPIC  Ask the device for only this topic (repeatable),\n"
                      << "                     e.g. tns1:RuleEngine/CellMotionDetector/Motion\n"
                      << "  --topic TEXT       Show only events whose topic contains TEXT\n"
                      << "Credentials: --username NAME --password PASSWORD; environment and"
                         " prompt fallbacks apply.\n";
            return 0;
        }
        if ((argument == "--url" || argument == "--ip" || argument == "--topic" ||
             argument == "--count" || argument == "--subscribe") &&
            index + 1 >= parsed.value().remaining.size())
        {
            std::cerr << "Missing value for " << argument << '\n';
            return 2;
        }
        if (argument == "--url")
        {
            url = parsed.value().remaining[++index];
        }
        else if (argument == "--ip")
        {
            address = parsed.value().remaining[++index];
        }
        else if (argument == "--topic")
        {
            topic_filter = parsed.value().remaining[++index];
        }
        else if (argument == "--subscribe")
        {
            subscribe.push_back(parsed.value().remaining[++index]);
        }
        else if (argument == "--topics")
        {
            list_topics = true;
        }
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
    if (!runtime_result)
    {
        return 1;
    }
    auto runtime = runtime_result.value();
    auto endpoint =
        wlonvif::examples::resolve_device_endpoint(url, address, runtime, common.timeout_seconds);
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
    auto event_client = session.value()->event_client();
    if (!event_client)
    {
        std::cerr << event_client.error().message << '\n';
        return 1;
    }
    // --topics: what the camera says it can send, then exit.
    if (list_topics)
    {
        auto topics = wlonvif::examples::wait_for_task(event_client.value()->get_topics(
            wlonvif::examples::call_options(common.timeout_seconds)
        ));
        if (!topics)
        {
            std::cerr << "Topic query failed: " << topics.error().message << '\n';
            (void)runtime->shutdown();
            return 1;
        }
        for (std::size_t index = 0; index < topics.value().size(); ++index)
        {
            const auto& topic = topics.value()[index];
            const auto names = [](const std::vector<std::string>& values)
            {
                std::string output;
                for (const auto& value : values)
                {
                    output += (output.empty() ? "" : ", ") + value;
                }
                return output;
            };
            if (common.json)
            {
                std::cout << (index ? "," : "[") << "{\"topic\":\""
                          << wlonvif::examples::json_escape(topic.topic)
                          << "\",\"property\":" << (topic.property ? "true" : "false") << '}';
                continue;
            }
            std::cout << topic.topic << (topic.property ? "  (property)" : "") << '\n';
            if (!topic.source.empty())
            {
                std::cout << "    source: " << names(topic.source) << '\n';
            }
            if (!topic.data.empty())
            {
                std::cout << "    data:   " << names(topic.data) << '\n';
            }
        }
        if (common.json)
        {
            std::cout << (topics.value().empty() ? "[" : "") << "]\n";
        }
        (void)runtime->shutdown();
        return 0;
    }
    // Events are queued while this loop is busy; past 256 the oldest are dropped
    // and a health signal says so.
    wlonvif::ManagedEventOptions event_options;
    event_options.queue_capacity = 256;
    event_options.topics = subscribe;
    auto stream = wlonvif::examples::wait_for_task(event_client.value()->open_managed_stream(
        event_options, wlonvif::examples::call_options(common.timeout_seconds)
    ));
    if (!stream)
    {
        std::cerr << stream.error().message << '\n';
        return 1;
    }
    std::size_t delivered = 0;
    while (count == 0 || delivered < count)
    {
        auto item = wlonvif::examples::wait_for_task(
            stream.value()->next(wlonvif::examples::call_options(common.timeout_seconds))
        );
        if (!item)
        {
            if (item.error().category == wlonvif::ErrorCategory::cancelled)
            {
                break;
            }
            // No event within the timeout is normal for a quiet camera.
            if (item.error().category == wlonvif::ErrorCategory::timeout)
            {
                continue;
            }
            std::cerr << "Event stream failed: " << item.error().message << '\n';
            break;
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
        {
            continue;
        }
        if (common.json)
        {
            std::cout << "{\"topic\":\"" << wlonvif::examples::json_escape(event.topic)
                      << "\",\"source\":\"" << wlonvif::examples::json_escape(event.source)
                      << "\",\"data\":\"" << wlonvif::examples::json_escape(event.data)
                      << "\",\"sequence\":" << event.sequence << "}\n";
        }
        else
        {
            std::cout << event.sequence << "  " << event.topic << "  " << event.source << "  "
                      << event.data << '\n';
        }
        ++delivered;
    }
    auto closed = wlonvif::examples::wait_for_task(
        stream.value()->close(wlonvif::examples::safety_call_options(5))
    );
    if (!closed && closed.error().category != wlonvif::ErrorCategory::cancelled)
    {
        std::cerr << "Unsubscribe failed: " << closed.error().message << '\n';
    }
    (void)runtime->shutdown();
    return 0;
}

/**
 * @file events_api.cpp
 * @brief The Events tab: topic list and a background event monitor per camera.
 *
 * The monitor thread pulls from a managed event stream, which renews and
 * re-creates the subscription by itself, and keeps the newest 500 entries. The
 * page polls for entries with a sequence number above the last one it has.
 */

#include "implementation.hpp"

#include <utility>

namespace wlonvif::examples::explorer
{
    namespace
    {
        constexpr std::size_t kept_events = 500;

        /// The log entry for one stream item: an event, or a note about its health.
        json::object event_entry(const EventOrHealth& item)
        {
            if (const auto* event = std::get_if<Event>(&item))
            {
                return to_json(*event).as_object();
            }
            const bool resubscribed =
                std::get<StreamHealth>(item) == StreamHealth::possible_event_loss;
            return json::object{
                {"health",
                 resubscribed ? "possible event loss (resubscribed)"
                              : "events dropped (queue full)"},
                {"time", utc_text(std::chrono::system_clock::now())}
            };
        }
    } // namespace

    Reply Explorer::Implementation::topics_view(Camera& camera, DeviceSession& session)
    {
        auto client = session.event_client();
        if (!client)
        {
            return failure(client.error());
        }
        auto topics = run(camera.id, "GetEventProperties", client.value()->get_topics(call()));
        return topics ? success(to_json(topics.value())) : failure(topics.error());
    }

    Reply Explorer::Implementation::start_events(
        const std::shared_ptr<Camera>& camera, DeviceSession& session, const json::object& body
    )
    {
        stop_events(*camera);
        auto client = session.event_client();
        if (!client)
        {
            return failure(client.error());
        }

        // No topics means every topic.
        ManagedEventOptions options;
        options.pull_timeout = std::chrono::milliseconds(2000);
        if (const auto topics = body.find("topics");
            topics != body.end() && topics->value().is_array())
        {
            for (const auto& topic : topics->value().as_array())
            {
                if (topic.is_string() && !topic.as_string().empty())
                {
                    options.topics.emplace_back(topic.as_string());
                }
            }
        }
        auto stream =
            run(camera->id,
                "CreatePullPointSubscription",
                client.value()->open_managed_stream(options, call()));
        if (!stream)
        {
            return failure(stream.error());
        }

        auto stop = std::make_shared<std::atomic_bool>(false);
        std::lock_guard lock(camera->event_mutex);
        camera->events = stream.value();
        camera->event_stop = stop;
        camera->event_error.reset();
        camera->event_topics = options.topics;
        camera->event_thread = std::thread(
            [camera, stream = stream.value(), stop]
            {
                while (!stop->load())
                {
                    // A short wait, so a stop request is noticed within a second.
                    const CallOptions wait{
                        SteadyClock::now() + std::chrono::seconds(1), CancellationSource{}.token()
                    };
                    auto item = examples::wait_for_task(stream->next(wait));
                    if (!item)
                    {
                        if (item.error().category == ErrorCategory::timeout)
                        {
                            continue;
                        }
                        if (item.error().category != ErrorCategory::cancelled && !stop->load())
                        {
                            std::lock_guard guard(camera->event_mutex);
                            camera->event_error = error_json(item.error());
                        }
                        break;
                    }
                    auto entry = event_entry(item.value());
                    std::lock_guard guard(camera->event_mutex);
                    entry["sequence"] = ++camera->event_sequence;
                    camera->event_log.push_back(std::move(entry));
                    while (camera->event_log.size() > kept_events)
                    {
                        camera->event_log.pop_front();
                    }
                }
            }
        );
        return success();
    }

    void Explorer::Implementation::stop_events(Camera& camera)
    {
        std::shared_ptr<ManagedEventStream> stream;
        std::thread thread;
        {
            std::lock_guard lock(camera.event_mutex);
            if (camera.event_stop)
            {
                camera.event_stop->store(true);
            }
            stream = std::exchange(camera.events, nullptr);
            thread = std::move(camera.event_thread);
        }
        // Join without the lock: the thread takes it to store each event.
        if (thread.joinable())
        {
            thread.join();
        }
        if (stream)
        {
            (void)examples::wait_for_task(stream->close(examples::call_options(3)));
        }
    }

    Reply Explorer::Implementation::clear_events(Camera& camera)
    {
        // Sequence numbers keep counting, so a page polling "since" stays correct.
        std::lock_guard lock(camera.event_mutex);
        camera.event_log.clear();
        return success();
    }

    Reply Explorer::Implementation::events_view(Camera& camera, std::uint64_t since)
    {
        std::lock_guard lock(camera.event_mutex);
        json::array events;
        for (const auto& entry : camera.event_log)
        {
            if (entry.at("sequence").to_number<std::uint64_t>() > since)
            {
                events.push_back(entry);
            }
        }
        json::object output{
            {"running", camera.events != nullptr},
            {"events", events},
            {"topics", to_json(camera.event_topics)}
        };
        if (camera.event_error)
        {
            output["error"] = *camera.event_error;
        }
        return success(output);
    }
} // namespace wlonvif::examples::explorer

/**
 * @file cameras.cpp
 * @brief The camera list, sign-in, network scan, request log, and Overview tab.
 */

#include "implementation.hpp"

#include <utility>
#include <wlonvif/version.hpp>

namespace wlonvif::examples::explorer
{
    Explorer::Implementation::Implementation(Explorer& explorer) : owner(explorer)
    {
        watchdog = std::thread(
            [this]
            {
                watch_direct_moves();
            }
        );
    }

    // ---- Calling cameras -------------------------------------------------------

    CallOptions Explorer::Implementation::call() const
    {
        auto options = examples::call_options(owner.options_.timeout_seconds);
        options.cancellation =
            CancellationToken::linked(options.cancellation, cancellation.token());
        return options;
    }

    void Explorer::Implementation::record(
        const std::string& camera,
        std::string action,
        SteadyClock::duration elapsed,
        const std::optional<Error>& error
    )
    {
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
        json::object entry{
            {"time", utc_text(std::chrono::system_clock::now())},
            {"camera", camera},
            {"action", std::move(action)},
            {"ms", milliseconds.count()},
            {"ok", !error}
        };
        if (error)
        {
            entry["error"] = error_json(*error);
        }
        std::lock_guard lock(log_mutex);
        entry["sequence"] = ++log_sequence;
        log.push_back(std::move(entry));
        while (log.size() > 500)
        {
            log.pop_front();
        }
    }

    Reply Explorer::Implementation::log_view(std::uint64_t since)
    {
        std::lock_guard lock(log_mutex);
        json::array entries;
        for (const auto& entry : log)
        {
            if (entry.at("sequence").to_number<std::uint64_t>() > since)
            {
                entries.push_back(entry);
            }
        }
        return success(entries);
    }

    // ---- Camera list -----------------------------------------------------------

    std::shared_ptr<Camera> Explorer::Implementation::camera(const std::string& id)
    {
        std::lock_guard lock(mutex);
        const auto found = cameras.find(id);
        return found == cameras.end() ? nullptr : found->second;
    }

    std::vector<std::shared_ptr<Camera>> Explorer::Implementation::all_cameras()
    {
        std::lock_guard lock(mutex);
        std::vector<std::shared_ptr<Camera>> list;
        for (auto& [_, camera] : cameras)
        {
            list.push_back(camera);
        }
        return list;
    }

    std::shared_ptr<Camera> Explorer::Implementation::add_camera(
        const std::string& address, const std::string& device_url, bool manual
    )
    {
        std::lock_guard lock(mutex);
        for (auto& [_, existing] : cameras)
        {
            const bool same_url = !device_url.empty() && existing->device_url == device_url;
            if (existing->address == address || same_url)
            {
                // A scan may learn the URL of a camera the user added by address.
                if (!device_url.empty() && existing->device_url.empty())
                {
                    existing->device_url = device_url;
                }
                return existing;
            }
        }
        auto created = std::make_shared<Camera>();
        created->id = "camera-" + std::to_string(next_camera++);
        created->address = address;
        created->device_url = device_url;
        created->manual = manual;
        created->credentials = owner.options_.default_credentials;
        cameras.emplace(created->id, created);
        return created;
    }

    json::object Explorer::Implementation::summary(Camera& camera)
    {
        json::object output{
            {"id", camera.id},
            {"address", camera.address},
            {"url", camera.device_url},
            {"evidence", camera.evidence},
            {"name", scope_value(camera.scopes, "name")},
            {"hardware", scope_value(camera.scopes, "hardware")},
            {"location", scope_value(camera.scopes, "location")},
            {"manual", camera.manual}
        };
        std::lock_guard lock(camera.mutex);
        output["connected"] = camera.session != nullptr;
        output["username"] = camera.credentials ? camera.credentials->username : std::string();
        if (camera.identity)
        {
            output["identity"] = to_json(*camera.identity);
        }
        if (camera.session && camera.capabilities)
        {
            // Lets the page dim tabs for services the camera does not offer. The
            // library finds services the same way, so a missing one cannot work.
            const auto& value = *camera.capabilities;
            output["services"] = json::object{
                {"media", value.media_xaddr.has_value()},
                {"ptz", value.ptz_xaddr.has_value()},
                {"events", value.events_xaddr.has_value()},
                {"analytics", value.analytics_xaddr.has_value()},
                {"imaging", value.imaging_xaddr.has_value()}
            };
        }
        if (camera.connection_error)
        {
            output["error"] = *camera.connection_error;
        }
        return output;
    }

    Reply Explorer::Implementation::state_view()
    {
        json::array list;
        for (auto& camera : all_cameras())
        {
            list.push_back(summary(*camera));
        }
        json::object scan;
        {
            std::lock_guard lock(scan_mutex);
            scan = scan_status;
        }
        const auto& defaults = owner.options_.default_credentials;
        return success(
            json::object{
                {"cameras", list},
                {"scan", scan},
                {"library", WLONVIF_VERSION_STRING},
                {"ffmpeg", !owner.options_.ffmpeg.empty()},
                {"recordings", !owner.options_.recordings.empty()},
                {"default_username", defaults ? defaults->username : std::string()}
            }
        );
    }

    Reply Explorer::Implementation::add_camera_request(const json::object& body)
    {
        const auto address = text_field(body, "address").value_or("");
        const auto url = text_field(body, "url").value_or("");
        if (address.empty() && url.empty())
        {
            return rejection(400, "address or url is required");
        }
        // Without an address, use the host part of the URL.
        std::string host = address;
        if (host.empty())
        {
            const auto start = url.find("://");
            host = start == std::string::npos ? url : url.substr(start + 3);
            host = host.substr(0, host.find_first_of(":/"));
        }
        auto added = add_camera(host, url, true);
        if (text_field(body, "username"))
        {
            return connect(*added, body);
        }
        return success(summary(*added));
    }

    Reply Explorer::Implementation::forget(const std::shared_ptr<Camera>& camera)
    {
        disconnect(*camera);
        {
            std::lock_guard lock(camera->check_mutex);
            if (camera->check_running)
            {
                return rejection(409, "a compatibility check is running");
            }
        }
        if (camera->check_thread.joinable())
        {
            camera->check_thread.join();
        }
        std::lock_guard lock(mutex);
        cameras.erase(camera->id);
        return success();
    }

    // ---- Sign-in ---------------------------------------------------------------

    std::shared_ptr<DeviceSession> Explorer::Implementation::session_of(Camera& camera)
    {
        std::lock_guard lock(camera.mutex);
        return camera.session;
    }

    std::optional<Credentials> Explorer::Implementation::credentials_of(Camera& camera)
    {
        std::lock_guard lock(camera.mutex);
        return camera.credentials;
    }

    Reply Explorer::Implementation::connect(Camera& camera, const json::object& body)
    {
        // A username in the request replaces the stored credentials; an
        // explicitly empty one means anonymous access.
        auto credentials = credentials_of(camera);
        const auto username = text_field(body, "username");
        const auto password = text_field(body, "password");
        if (username && !username->empty())
        {
            credentials = Credentials{*username, password.value_or(""), false};
        }
        else if (username)
        {
            credentials.reset();
        }

        if (camera.device_url.empty())
        {
            auto resolved = timed(
                camera.id,
                "Resolve Device service",
                [&]
                {
                    return owner.resolver_(camera.address);
                }
            );
            if (!resolved)
            {
                return failure(resolved.error());
            }
            camera.device_url = resolved.value();
        }

        disconnect(camera);
        auto session = timed(
            camera.id,
            "Connect",
            [&]
            {
                return owner.connector_(camera.device_url, credentials);
            }
        );
        {
            std::lock_guard lock(camera.mutex);
            camera.credentials = credentials;
            if (!session)
            {
                camera.connection_error = error_json(session.error());
                return failure(session.error());
            }
            camera.session = session.value();
            camera.connection_error.reset();
        }

        // What the sidebar shows, and which services the tabs can use.
        auto& device = *session.value();
        auto identity =
            run(camera.id, "GetDeviceInformation", device.get_device_information(call()));
        auto scopes = run(camera.id, "GetScopes", device.get_scopes(call()));
        auto capabilities = run(camera.id, "GetCapabilities", device.get_capabilities(call()));
        {
            std::lock_guard lock(camera.mutex);
            if (identity)
            {
                camera.identity = identity.value();
            }
            // Unknown when the camera will not say; the page then assumes everything.
            camera.capabilities =
                capabilities ? std::optional<Capabilities>(capabilities.value()) : std::nullopt;
        }
        if (scopes && camera.scopes.empty())
        {
            camera.scopes = scopes.value();
        }
        return success(summary(camera));
    }

    void Explorer::Implementation::disconnect(Camera& camera)
    {
        stop_events(camera);
        stop_video(camera);
        close_controllers(camera);
        std::shared_ptr<DeviceSession> session;
        {
            std::lock_guard lock(camera.mutex);
            session = std::exchange(camera.session, nullptr);
        }
        if (session)
        {
            (void)examples::wait_for_task(session->close(std::chrono::seconds(2)));
        }
    }

    // ---- Network scan ----------------------------------------------------------

    Reply Explorer::Implementation::start_scan(const json::object& body)
    {
        std::lock_guard lock(scan_mutex);
        if (scanning)
        {
            return success(scan_status);
        }
        if (scan_thread.joinable())
        {
            scan_thread.join();
        }

        // Scan the requested networks, or every local IPv4 network.
        NetworkDiscoveryOptions options;
        options.direct_probe = true;
        options.maximum_hosts = owner.options_.maximum_hosts;
        if (const auto networks = body.find("networks");
            networks != body.end() && networks->value().is_array())
        {
            for (const auto& network : networks->value().as_array())
            {
                if (network.is_string())
                {
                    options.scan_networks.emplace_back(network.as_string());
                }
            }
        }
        if (options.scan_networks.empty())
        {
            auto local = local_ipv4_networks(owner.options_.maximum_hosts);
            if (local)
            {
                options.scan_networks = local.value();
            }
        }
        // The overall limit leaves every address its full per-address time.
        auto bound = discovery_time_bound(options);
        if (!bound)
        {
            return failure(bound.error());
        }

        json::array networks;
        for (const auto& network : options.scan_networks)
        {
            networks.push_back(json::string(network));
        }
        scanning = true;
        scan_status = json::object{
            {"running", true},
            {"stage", "starting"},
            {"completed", 0},
            {"total", 0},
            {"networks", networks},
            {"started", utc_text(std::chrono::system_clock::now())},
            {"limit_seconds", std::chrono::ceil<std::chrono::seconds>(bound.value()).count()}
        };
        options.progress = [this](const DiscoveryProgress& progress)
        {
            std::lock_guard guard(scan_mutex);
            const bool multicast = progress.stage == DiscoveryProgressStage::ws_discovery;
            scan_status["stage"] = multicast ? "ws-discovery" : "direct-probe";
            scan_status["completed"] = progress.completed;
            scan_status["total"] = progress.total;
            scan_status["current"] = progress.current;
        };

        const auto deadline = SteadyClock::now() + bound.value();
        scan_thread = std::thread(
            [this, options = std::move(options), deadline]() mutable
            {
                const auto started = SteadyClock::now();
                auto report =
                    owner.scanner_(std::move(options), CallOptions{deadline, cancellation.token()});
                record("", "Network scan", SteadyClock::now() - started, error_of(report));

                json::object finished{
                    {"running", false}, {"finished", utc_text(std::chrono::system_clock::now())}
                };
                if (!report)
                {
                    finished["error"] = error_json(report.error());
                }
                else
                {
                    for (const auto& device : report.value().devices)
                    {
                        auto found =
                            add_camera(device.address, device.device_service.value_or(""), false);
                        std::lock_guard guard(mutex);
                        found->evidence = evidence_name(device.evidence);
                        if (device.discovery && !device.discovery->scopes.empty())
                        {
                            found->scopes = device.discovery->scopes;
                        }
                    }
                    finished["found"] = report.value().devices.size();
                    // Addresses the time limit cut short, so the page can say so.
                    finished["unchecked"] = to_json(report.value().unchecked);
                    finished["incomplete"] = to_json(report.value().incomplete);
                }
                std::lock_guard guard(scan_mutex);
                for (auto& [key, value] : finished)
                {
                    scan_status[key] = value;
                }
                scanning = false;
            }
        );
        return success(scan_status);
    }

    // ---- Overview tab ----------------------------------------------------------

    Reply Explorer::Implementation::overview(Camera& camera, DeviceSession& session)
    {
        const auto services = [](const std::vector<Service>& list)
        {
            json::array output;
            for (const auto& service : list)
            {
                output.push_back(
                    json::object{
                        {"namespace", service.namespace_uri},
                        {"xaddr", service.xaddr},
                        {"support", service_support(service.namespace_uri)}
                    }
                );
            }
            return output;
        };
        json::object output;
        output["identity"] =
            section(run(camera.id, "GetDeviceInformation", session.get_device_information(call())));
        output["clock"] = section(
            run(camera.id, "GetSystemDateAndTime", session.get_system_date_and_time(call()))
        );
        output["capabilities"] =
            section(run(camera.id, "GetCapabilities", session.get_capabilities(call())));
        output["services"] =
            section(run(camera.id, "GetServices", session.get_services(call())), services);
        output["scopes"] = section(run(camera.id, "GetScopes", session.get_scope_entries(call())));
        return success(output);
    }

    // ---- Shutdown --------------------------------------------------------------

    void Explorer::Implementation::shutdown()
    {
        // The watchdog goes first: it calls cameras that are about to close.
        watchdog_stop.store(true);
        if (watchdog.joinable())
        {
            watchdog.join();
        }
        cancellation.request_stop();
        if (scan_thread.joinable())
        {
            scan_thread.join();
        }
        for (auto& camera : all_cameras())
        {
            if (camera->check_thread.joinable())
            {
                camera->check_thread.join();
            }
            disconnect(*camera);
        }
        std::lock_guard lock(recording_mutex);
        for (auto& [_, recording] : recordings)
        {
            recording->stop();
        }
    }
} // namespace wlonvif::examples::explorer

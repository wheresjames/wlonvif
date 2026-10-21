/**
 * @file device_api.cpp
 * @brief The Device tab (clock, NTP, network, scopes, reboot) and the Raw request tab.
 */

#include "implementation.hpp"

namespace wlonvif::examples::explorer
{
    namespace
    {
        template <class T> Reply done(const Result<T>& result)
        {
            return result ? success() : failure(result.error());
        }

        /// The strings in an array field, skipping empty ones and other types.
        std::vector<std::string> text_list(const json::object& body, std::string_view key)
        {
            std::vector<std::string> output;
            const auto found = body.find(key);
            if (found == body.end() || !found->value().is_array())
            {
                return output;
            }
            for (const auto& item : found->value().as_array())
            {
                if (item.is_string() && !item.as_string().empty())
                {
                    output.emplace_back(item.as_string());
                }
            }
            return output;
        }
    } // namespace

    Reply Explorer::Implementation::device_view(Camera& camera, DeviceSession& session)
    {
        // Each setting is its own section: cameras implement these unevenly.
        json::object output;
        output["clock"] = section(
            run(camera.id, "GetSystemDateAndTime", session.get_system_date_and_time(call()))
        );
        output["ntp"] = section(run(camera.id, "GetNTP", session.get_ntp(call())));
        output["scopes"] = section(run(camera.id, "GetScopes", session.get_scope_entries(call())));
        output["hostname"] = section(run(camera.id, "GetHostname", session.get_hostname(call())));
        output["interfaces"] =
            section(run(camera.id, "GetNetworkInterfaces", session.get_network_interfaces(call())));
        output["gateway"] = section(
            run(camera.id, "GetNetworkDefaultGateway", session.get_network_default_gateway(call()))
        );
        output["dns"] = section(run(camera.id, "GetDNS", session.get_dns(call())));
        output["protocols"] =
            section(run(camera.id, "GetNetworkProtocols", session.get_network_protocols(call())));
        output["discoverable"] =
            section(run(camera.id, "GetDiscoveryMode", session.get_discovery_mode(call())));
        output["users"] = section(run(camera.id, "GetUsers", session.get_users(call())));
        return success(output);
    }

    Reply Explorer::Implementation::device_action(
        Camera& camera, DeviceSession& session, const json::object& body
    )
    {
        const auto action = text_field(body, "action").value_or("");

        // sync_time: set the camera clock to this computer's (manual mode).
        // ntp_mode:  let the camera take its time from NTP.
        // time_zone: change the zone and daylight saving, keeping the mode.
        if (action == "sync_time" || action == "time_zone" || action == "ntp_mode")
        {
            auto current =
                run(camera.id, "GetSystemDateAndTime", session.get_system_date_and_time(call()));
            if (!current)
            {
                return failure(current.error());
            }
            const auto now = std::chrono::system_clock::now();
            SystemDateAndTimeSettings settings;
            settings.type = action == "sync_time"  ? DateTimeType::manual
                            : action == "ntp_mode" ? DateTimeType::ntp
                                                   : current.value().type;
            settings.daylight_savings =
                flag_field(body, "dst").value_or(current.value().daylight_savings);
            settings.time_zone = text_field(body, "time_zone");
            if (settings.type == DateTimeType::manual)
            {
                // A manual clock needs a time: ours when syncing, else the camera's own,
                // carried forward by the time this request took.
                settings.utc = action == "sync_time" ? now : now + (current.value().utc - now);
            }
            return done(
                run(camera.id,
                    "SetSystemDateAndTime",
                    session.set_system_date_and_time(settings, call()))
            );
        }

        if (action == "ntp_servers")
        {
            NtpConfiguration configuration;
            configuration.from_dhcp = flag_field(body, "dhcp").value_or(false);
            configuration.servers = text_list(body, "servers");
            return done(run(camera.id, "SetNTP", session.set_ntp(configuration, call())));
        }

        if (action == "add_scope")
        {
            const auto scope = text_field(body, "scope").value_or("");
            return done(run(camera.id, "AddScopes", session.add_scopes({scope}, call())));
        }
        if (action == "remove_scope")
        {
            const auto scope = text_field(body, "scope").value_or("");
            return done(run(camera.id, "RemoveScopes", session.remove_scopes({scope}, call())));
        }

        if (action == "hostname")
        {
            const auto name = text_field(body, "name").value_or("");
            return done(run(camera.id, "SetHostname", session.set_hostname(name, call())));
        }

        if (action == "discoverable")
        {
            const auto mode = flag_field(body, "enabled").value_or(true)
                                  ? DiscoveryMode::discoverable
                                  : DiscoveryMode::non_discoverable;
            return done(
                run(camera.id, "SetDiscoveryMode", session.set_discovery_mode(mode, call()))
            );
        }

        if (action == "reboot")
        {
            // The page asks first; the flag keeps a stray request from rebooting.
            if (!flag_field(body, "confirm").value_or(false))
            {
                return failure(invalid("reboot requires confirmation"));
            }
            auto message = run(camera.id, "SystemReboot", session.system_reboot(call()));
            return message ? success(json::value(message.value())) : failure(message.error());
        }
        return rejection(400, "unknown device action");
    }

    Reply Explorer::Implementation::raw_call(
        Camera& camera, DeviceSession& session, const json::object& body
    )
    {
        // The request goes to the endpoint the camera advertises for the service,
        // signed like any other request.
        const std::map<std::string, OnvifService> services{
            {"device", OnvifService::device},
            {"media", OnvifService::media},
            {"ptz", OnvifService::ptz},
            {"events", OnvifService::events},
            {"analytics", OnvifService::analytics},
            {"imaging", OnvifService::imaging}
        };
        const auto name = text_field(body, "service").value_or("device");
        const auto service = services.find(name);
        if (service == services.end())
        {
            return rejection(400, "unknown service " + name);
        }
        auto answer =
            run(camera.id,
                "Custom request",
                session.call(
                    service->second,
                    text_field(body, "body").value_or(""),
                    text_field(body, "response").value_or(""),
                    call()
                ));
        return answer ? success(json::value(answer.value())) : failure(answer.error());
    }
} // namespace wlonvif::examples::explorer

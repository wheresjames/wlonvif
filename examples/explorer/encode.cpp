/**
 * @file encode.cpp
 * @brief JSON encodings of the library's result types, as the Explorer page reads them.
 *
 * Optional values become JSON null rather than being left out, so the page
 * can tell "not reported" from "zero".
 */

#include "encode.hpp"

#include <cmath>
#include <ctime>
#include <example_support.hpp>

namespace wlonvif::examples::explorer
{
    namespace
    {
        json::value optional_number(const std::optional<double>& value)
        {
            return value && std::isfinite(*value) ? json::value(*value) : json::value(nullptr);
        }

        json::value optional_text(const std::optional<std::string>& value)
        {
            return value ? json::value(*value) : json::value(nullptr);
        }

        json::value range(const NativeRange& value)
        {
            return json::object{{"min", value.minimum}, {"max", value.maximum}};
        }

        json::value range(const IntegerRange& value)
        {
            return json::object{{"min", value.minimum}, {"max", value.maximum}};
        }

        json::value range(const std::optional<ImagingRange>& value)
        {
            if (!value)
            {
                return nullptr;
            }
            return json::object{{"min", value->minimum}, {"max", value->maximum}};
        }

        json::value vector_json(const PtzVector& value)
        {
            return json::object{
                {"pan", optional_number(value.pan)},
                {"tilt", optional_number(value.tilt)},
                {"zoom", optional_number(value.zoom)}
            };
        }

        json::value configs_json(const std::vector<AnalyticsConfig>& value)
        {
            json::array output;
            for (const auto& config : value)
            {
                json::array simple, structured;
                for (const auto& item : config.simple_items)
                {
                    simple.push_back(json::object{{"name", item.name}, {"value", item.value}});
                }
                for (const auto& item : config.element_items)
                {
                    structured.push_back(json::object{{"name", item.name}, {"xml", item.xml}});
                }
                output.push_back(
                    json::object{
                        {"name", config.name},
                        {"type", config.type},
                        {"parameters", simple},
                        {"structured", structured}
                    }
                );
            }
            return output;
        }
    } // namespace

    json::object error_json(const Error& error)
    {
        json::object output{
            {"category", std::string(examples::error_category_name(error.category))},
            {"operation", std::string(examples::operation_name(error.operation))},
            {"message", redact_credentials(error.message)},
            {"retryable", error.retryable},
            {"may_have_applied", error.request_may_have_been_applied},
        };
        if (error.http)
        {
            output["http_status"] = error.http->status;
        }
        if (error.soap_fault)
        {
            const auto& fault = *error.soap_fault;
            output["soap"] = json::object{
                {"code", fault.code},
                {"subcode", fault.subcode},
                {"reason", fault.reason},
                {"detail", fault.detail}
            };
        }
        if (error.host_permission)
        {
            output["host"] = json::object{
                {"hostname", error.host_permission->hostname}, {"port", error.host_permission->port}
            };
        }
        if (error.safety_action)
        {
            output["safety_stop"] = json::object{
                {"attempted", error.safety_action->attempted},
                {"succeeded", error.safety_action->succeeded}
            };
        }
        return output;
    }

    std::string utc_text(std::chrono::system_clock::time_point time)
    {
        const auto seconds = std::chrono::system_clock::to_time_t(time);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &seconds);
#else
        gmtime_r(&seconds, &utc);
#endif
        char text[32]{};
        std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &utc);
        return text;
    }

    std::string redact_credentials(std::string text)
    {
        for (std::size_t scheme = text.find("://"); scheme != std::string::npos;
             scheme = text.find("://", scheme + 3))
        {
            const auto start = scheme + 3;
            const auto end = text.find_first_of("/ \t\r\n\"'", start);
            const auto at = text.find('@', start);
            if (at != std::string::npos && (end == std::string::npos || at < end))
            {
                text.replace(start, at - start, "***");
            }
        }
        return text;
    }

    json::value to_json(const DeviceInformation& value)
    {
        return json::object{
            {"manufacturer", value.manufacturer},
            {"model", value.model},
            {"firmware", value.firmware_version},
            {"serial", value.serial_number},
            {"hardware", value.hardware_id}
        };
    }

    json::value to_json(const SystemDateAndTime& value)
    {
        const auto skew =
            std::chrono::round<std::chrono::seconds>(value.utc - std::chrono::system_clock::now());
        return json::object{
            {"utc", utc_text(value.utc)},
            {"skew_seconds", skew.count()},
            {"mode", value.type == DateTimeType::ntp ? "ntp" : "manual"},
            {"daylight_savings", value.daylight_savings},
            {"time_zone", value.time_zone}
        };
    }

    json::value to_json(const NtpConfiguration& value)
    {
        return json::object{
            {"from_dhcp", value.from_dhcp},
            {"servers", to_json(value.servers)},
            {"dhcp_servers", to_json(value.dhcp_servers)}
        };
    }

    json::value to_json(const std::vector<ScopeEntry>& value)
    {
        json::array output;
        for (const auto& scope : value)
        {
            output.push_back(
                json::object{
                    {"uri", scope.uri}, {"fixed", scope.definition == ScopeDefinition::fixed}
                }
            );
        }
        return output;
    }

    json::value to_json(const std::vector<Service>& value)
    {
        json::array output;
        for (const auto& service : value)
        {
            output.push_back(
                json::object{{"namespace", service.namespace_uri}, {"xaddr", service.xaddr}}
            );
        }
        return output;
    }

    json::value to_json(const Capabilities& value)
    {
        return json::object{
            {"media", optional_text(value.media_xaddr)},
            {"ptz", optional_text(value.ptz_xaddr)},
            {"events", optional_text(value.events_xaddr)},
            {"analytics", optional_text(value.analytics_xaddr)},
            {"imaging", optional_text(value.imaging_xaddr)}
        };
    }

    json::value to_json(const VideoEncoderConfiguration& value)
    {
        return json::object{
            {"token", value.token},
            {"name", value.name},
            {"encoding", value.encoding},
            {"width", value.resolution.width},
            {"height", value.resolution.height},
            {"quality", value.quality},
            {"fps", value.frame_rate_limit},
            {"interval", value.encoding_interval},
            {"bitrate", value.bitrate_limit},
            {"gov", value.gov_length ? json::value(*value.gov_length) : json::value(nullptr)},
            {"profile", value.profile},
            {"session_timeout", value.session_timeout},
            {"multicast", value.multicast_address + ":" + std::to_string(value.multicast_port)},
        };
    }

    json::value to_json(const ProfileConfiguration& value)
    {
        json::object output{
            {"token", value.token},
            {"name", value.name},
            {"fixed", value.fixed},
            {"ptz", value.ptz_configuration},
            {"analytics", value.video_analytics_configuration},
            {"audio_source", value.audio_source}
        };
        if (value.video_source)
        {
            output["video_source"] = json::object{
                {"token", value.video_source->token},
                {"source", value.video_source->source_token},
                {"x", value.video_source->x},
                {"y", value.video_source->y},
                {"width", value.video_source->width},
                {"height", value.video_source->height}
            };
        }
        if (value.video_encoder)
        {
            output["video_encoder"] = to_json(*value.video_encoder);
        }
        if (value.audio_encoder)
        {
            output["audio_encoder"] = json::object{
                {"token", value.audio_encoder->token},
                {"encoding", value.audio_encoder->encoding},
                {"bitrate", value.audio_encoder->bitrate},
                {"sample_rate", value.audio_encoder->sample_rate}
            };
        }
        return output;
    }

    json::value to_json(const VideoEncoderOptions& value)
    {
        json::array encodings;
        for (const auto& encoding : value.encodings)
        {
            json::array sizes;
            for (const auto& size : encoding.resolutions)
            {
                sizes.push_back(json::object{{"width", size.width}, {"height", size.height}});
            }
            encodings.push_back(
                json::object{
                    {"encoding", encoding.encoding},
                    {"resolutions", sizes},
                    {"fps", range(encoding.frame_rate)},
                    {"interval", range(encoding.encoding_interval)},
                    {"gov",
                     encoding.gov_length ? range(*encoding.gov_length) : json::value(nullptr)},
                    {"bitrate", encoding.bitrate ? range(*encoding.bitrate) : json::value(nullptr)},
                    {"profiles", to_json(encoding.profiles)}
                }
            );
        }
        return json::object{{"quality", range(value.quality)}, {"encodings", encodings}};
    }

    json::value to_json(const VideoSource& value)
    {
        return json::object{
            {"token", value.token},
            {"framerate", value.framerate},
            {"width", value.width},
            {"height", value.height}
        };
    }

    json::value to_json(const ImagingSettings& value)
    {
        return json::object{
            {"brightness", optional_number(value.brightness)},
            {"contrast", optional_number(value.contrast)},
            {"saturation", optional_number(value.color_saturation)},
            {"sharpness", optional_number(value.sharpness)},
            {"ir_cut", optional_text(value.ir_cut_filter)},
            {"backlight", optional_text(value.backlight_compensation)},
            {"wdr", optional_text(value.wide_dynamic_range)},
            {"white_balance", optional_text(value.white_balance)}
        };
    }

    json::value to_json(const ImagingOptions& value)
    {
        return json::object{
            {"brightness", range(value.brightness)},
            {"contrast", range(value.contrast)},
            {"saturation", range(value.color_saturation)},
            {"sharpness", range(value.sharpness)},
            {"ir_cut", to_json(value.ir_cut_filter_modes)},
            {"backlight", to_json(value.backlight_compensation_modes)},
            {"wdr", to_json(value.wide_dynamic_range_modes)},
            {"white_balance", to_json(value.white_balance_modes)}
        };
    }

    json::value to_json(const PtzConfiguration& value)
    {
        return json::object{
            {"token", value.token},
            {"name", value.name},
            {"node", value.node_token},
            {"default_timeout", value.default_timeout.count()},
            {"eflip", optional_text(value.direction.eflip)},
            {"reverse", optional_text(value.direction.reverse)},
            {"eflip_modes", to_json(value.eflip_modes)},
            {"reverse_modes", to_json(value.reverse_modes)}
        };
    }

    json::value to_json(const PtzStatus& value)
    {
        return json::object{
            {"position", vector_json(value.position)},
            {"moving", value.moving},
            {"reliable", value.position_reliable}
        };
    }

    json::value to_json(const PtzSpaces& value)
    {
        return json::object{
            {"absolute", !value.absolute_pan_tilt.empty()},
            {"absolute_zoom", !value.absolute_zoom.empty()},
            {"relative", !value.relative_pan_tilt.empty()},
            {"relative_zoom", !value.relative_zoom.empty()},
            {"continuous", !value.continuous_pan_tilt.empty()},
            {"continuous_zoom", !value.continuous_zoom.empty()},
            {"pan", range(value.pan)},
            {"tilt", range(value.tilt)},
            {"zoom", range(value.zoom)}
        };
    }

    json::value to_json(const std::vector<Preset>& value)
    {
        json::array output;
        for (const auto& preset : value)
        {
            output.push_back(
                json::object{
                    {"token", preset.token},
                    {"name", preset.name},
                    {"position", vector_json(preset.position)}
                }
            );
        }
        return output;
    }

    json::value to_json(const std::vector<PresetTour>& value)
    {
        json::array output;
        for (const auto& tour : value)
        {
            json::array spots;
            for (const auto& spot : tour.spots)
            {
                spots.push_back(
                    json::object{{"preset", spot.preset}, {"stay", spot.stay_time.count()}}
                );
            }
            const auto state = tour.state == PresetTourState::idle      ? "idle"
                               : tour.state == PresetTourState::touring ? "touring"
                               : tour.state == PresetTourState::paused  ? "paused"
                                                                        : "unknown";
            output.push_back(
                json::object{
                    {"token", tour.token},
                    {"name", tour.name},
                    {"state", state},
                    {"auto_start", tour.auto_start},
                    {"spots", spots}
                }
            );
        }
        return output;
    }

    json::value to_json(const std::vector<AnalyticsConfig>& value)
    {
        return configs_json(value);
    }

    json::value to_json(const std::vector<EventTopic>& value)
    {
        json::array output;
        for (const auto& topic : value)
        {
            output.push_back(
                json::object{
                    {"topic", topic.topic},
                    {"property", topic.property},
                    {"source", to_json(topic.source)},
                    {"data", to_json(topic.data)}
                }
            );
        }
        return output;
    }

    json::value to_json(const Event& value)
    {
        return json::object{
            {"topic", value.topic},
            {"time", utc_text(value.utc_time)},
            {"source", value.source},
            {"data", value.data}
        };
    }

    json::value to_json(const HostnameInformation& value)
    {
        return json::object{{"name", value.name}, {"from_dhcp", value.from_dhcp}};
    }

    json::value to_json(const std::vector<NetworkInterface>& value)
    {
        json::array output;
        for (const auto& item : value)
        {
            output.push_back(
                json::object{
                    {"token", item.token},
                    {"name", item.name},
                    {"enabled", item.enabled},
                    {"mac", item.hardware_address},
                    {"mtu", item.mtu},
                    {"ipv4", to_json(item.ipv4_addresses)},
                    {"dhcp", item.ipv4_dhcp}
                }
            );
        }
        return output;
    }

    json::value to_json(const DnsInformation& value)
    {
        return json::object{
            {"from_dhcp", value.from_dhcp},
            {"servers", to_json(value.servers)},
            {"dhcp_servers", to_json(value.dhcp_servers)},
            {"search", to_json(value.search_domains)}
        };
    }

    json::value to_json(const std::vector<NetworkProtocol>& value)
    {
        json::array output;
        for (const auto& protocol : value)
        {
            json::array ports;
            for (const auto port : protocol.ports)
            {
                ports.push_back(port);
            }
            output.push_back(
                json::object{
                    {"name", protocol.name}, {"enabled", protocol.enabled}, {"ports", ports}
                }
            );
        }
        return output;
    }

    json::value to_json(const std::vector<DeviceUser>& value)
    {
        json::array output;
        for (const auto& user : value)
        {
            output.push_back(json::object{{"username", user.username}, {"level", user.level}});
        }
        return output;
    }

    json::value to_json(DiscoveryMode value)
    {
        return value == DiscoveryMode::discoverable;
    }

    json::value to_json(const std::vector<std::string>& value)
    {
        json::array output;
        for (const auto& item : value)
        {
            output.push_back(json::string(item));
        }
        return output;
    }

    Result<ImagingSettings> imaging_settings_from(const json::object& object)
    {
        ImagingSettings settings;
        const auto invalid = [](std::string message)
        {
            return Error{
                ErrorCategory::invalid_argument,
                OperationId::set_imaging_settings,
                std::move(message)
            };
        };
        for (const auto& [key, target] :
             {std::pair{"brightness", &settings.brightness},
              std::pair{"contrast", &settings.contrast},
              std::pair{"saturation", &settings.color_saturation},
              std::pair{"sharpness", &settings.sharpness}})
        {
            const auto found = object.find(key);
            if (found == object.end() || found->value().is_null())
            {
                continue;
            }
            if (!found->value().is_number())
            {
                return invalid(std::string(key) + " must be a number");
            }
            const double number = found->value().to_number<double>();
            if (!std::isfinite(number))
            {
                return invalid(std::string(key) + " must be finite");
            }
            *target = number;
        }
        for (const auto& [key, target] :
             {std::pair{"ir_cut", &settings.ir_cut_filter},
              std::pair{"backlight", &settings.backlight_compensation},
              std::pair{"wdr", &settings.wide_dynamic_range},
              std::pair{"white_balance", &settings.white_balance}})
        {
            const auto found = object.find(key);
            if (found == object.end() || found->value().is_null())
            {
                continue;
            }
            if (!found->value().is_string())
            {
                return invalid(std::string(key) + " must be text");
            }
            *target = std::string(found->value().as_string());
        }
        return settings;
    }
} // namespace wlonvif::examples::explorer

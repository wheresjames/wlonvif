#include "network_services.hpp"

#include "xml_scan.hpp"

#include <algorithm>
#include <condition_variable>
#include <ctime>
#include <map>
#include <sstream>

namespace wlonvif
{
    namespace
    {
        thread_local std::optional<CallOptions> active_call_options;
    }

    void set_network_call_options(const CallOptions& options)
    {
        active_call_options = options;
    }

    void clear_network_call_options()
    {
        active_call_options.reset();
    }

    namespace
    {
        /**
         * @brief Run a cold task to completion on the calling thread.
         *
         * The synchronization state is shared-owned rather than left on the stack.
         * With stack state the waiter can wake, return, and destroy the condition
         * variable while the completing thread is still inside notify_one(), which
         * ThreadSanitizer flags on every call. Notifying under the lock closes the
         * same window from the other side.
         */
        template <class T> T wait_for_task(Task<T> task)
        {
            struct SharedState
            {
                std::mutex mutex;
                std::condition_variable ready;
                std::optional<T> result;
            };
            auto state = std::make_shared<SharedState>();
            std::move(task).start(
                [state](T value)
                {
                    std::lock_guard lock(state->mutex);
                    state->result.emplace(std::move(value));
                    state->ready.notify_one();
                }
            );
            std::unique_lock lock(state->mutex);
            state->ready.wait(lock, [&] { return state->result.has_value(); });
            return std::move(*state->result);
        }

        using detail_xml::attribute;
        using detail_xml::numeric_attribute;

        std::string xml_escape(std::string_view input)
        {
            return detail_xml::escape(input);
        }

        /// Return the character data of the first element with local name @p name.
        std::string first_element(std::string_view xml, std::string_view local_name)
        {
            return detail_xml::first_text(xml, local_name);
        }

        /// Return the raw markup inside the first element with local name @p name.
        std::string subtree(std::string_view xml, std::string_view local_name)
        {
            const auto element = detail_xml::find_element(xml, local_name);
            return element ? std::string(detail_xml::inner(xml, *element)) : std::string{};
        }

        std::optional<NativeRange> first_range(std::string_view xml, std::string_view name)
        {
            const auto section = subtree(xml, name);
            if (section.empty()) return std::nullopt;
            const auto minimum = detail_xml::parse_double(first_element(section, "Min"));
            const auto maximum = detail_xml::parse_double(first_element(section, "Max"));
            if (!minimum || !maximum) return std::nullopt;
            return NativeRange{*minimum, *maximum};
        }

        /**
         * @brief Parse an ONVIF xs:dateTime such as `2024-05-06T07:08:09.5Z`.
         *
         * Returns nullopt for anything it cannot fully account for, so the caller
         * can fall back rather than reporting a fabricated timestamp.
         */
        std::optional<std::chrono::system_clock::time_point>
        parse_xsd_datetime(std::string_view text)
        {
            int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
            std::size_t index = 0;
            const auto number = [&](int& output, std::size_t digits)
            {
                if (index + digits > text.size()) return false;
                const auto parsed = std::from_chars(
                    text.data() + index, text.data() + index + digits, output
                );
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + index + digits)
                {
                    return false;
                }
                index += digits;
                return true;
            };
            const auto literal = [&](char expected)
            {
                if (index >= text.size() || text[index] != expected) return false;
                ++index;
                return true;
            };
            if (!number(year, 4) || !literal('-') || !number(month, 2) || !literal('-') ||
                !number(day, 2) || !literal('T') || !number(hour, 2) || !literal(':') ||
                !number(minute, 2) || !literal(':') || !number(second, 2))
            {
                return std::nullopt;
            }
            if (index < text.size() && text[index] == '.')
            {
                ++index;
                while (index < text.size() && text[index] >= '0' && text[index] <= '9') ++index;
            }
            std::chrono::minutes offset{0};
            if (index < text.size() && (text[index] == '+' || text[index] == '-'))
            {
                const int sign = text[index] == '-' ? -1 : 1;
                ++index;
                int offset_hours = 0, offset_minutes = 0;
                if (!number(offset_hours, 2) || !literal(':') || !number(offset_minutes, 2))
                {
                    return std::nullopt;
                }
                offset = sign * (std::chrono::hours(offset_hours) +
                                 std::chrono::minutes(offset_minutes));
            }
            else if (index < text.size() && (text[index] == 'Z' || text[index] == 'z'))
            {
                ++index;
            }
            if (index != text.size()) return std::nullopt;
            const auto date = std::chrono::year(year) /
                              std::chrono::month(static_cast<unsigned>(month)) /
                              std::chrono::day(static_cast<unsigned>(day));
            if (!date.ok() || hour > 23 || minute > 59 || second > 60) return std::nullopt;
            return std::chrono::system_clock::time_point(
                std::chrono::duration_cast<std::chrono::system_clock::duration>(
                    std::chrono::sys_days(date).time_since_epoch() + std::chrono::hours(hour) +
                    std::chrono::minutes(minute) + std::chrono::seconds(second) - offset
                )
            );
        }

        /// Join every SimpleItem in @p xml as comma-separated `name=value` pairs.
        std::string simple_items(std::string_view xml)
        {
            std::string output;
            detail_xml::for_each_element(
                xml,
                "SimpleItem",
                [&](const detail_xml::Element& item)
                {
                    const auto raw = detail_xml::attributes(xml, item);
                    const auto name = attribute(raw, "Name");
                    if (name.empty()) return;
                    if (!output.empty()) output += ',';
                    output += name + '=' + attribute(raw, "Value");
                }
            );
            return output;
        }

        class NetworkServiceBackend final : public ServiceBackend
        {
          public:
            NetworkServiceBackend(
                std::string device_url,
                std::shared_ptr<HttpTransport> transport,
                std::shared_ptr<EndpointPolicy> policy,
                std::shared_ptr<Runtime> runtime,
                std::optional<Credentials> credentials
            )
                : device_url_(std::move(device_url)), transport_(std::move(transport)),
                  policy_(std::move(policy)), runtime_(std::move(runtime)),
                  credentials_(std::move(credentials))
            {
            }

            Result<std::vector<MediaProfile>> profiles() override
            {
                auto endpoint = service_endpoint(Service::media);
                if (!endpoint) return endpoint.error();
                auto response = invoke(
                    endpoint.value(), OperationId::get_profiles, {}, "GetProfilesResponse"
                );
                if (!response) return response.error();
                std::vector<MediaProfile> output;
                const std::string& xml = response.value();
                detail_xml::for_each_element(
                    xml,
                    "Profiles",
                    [&](const detail_xml::Element& profile)
                    {
                        const auto token =
                            attribute(detail_xml::attributes(xml, profile), "token");
                        const auto body = std::string(detail_xml::inner(xml, profile));
                        output.push_back({token, first_element(body, "Name")});
                        const auto configuration =
                            detail_xml::find_element(body, "PTZConfiguration");
                        if (configuration)
                        {
                            std::lock_guard lock(profile_mutex_);
                            ptz_configurations_[token] =
                                attribute(detail_xml::attributes(body, *configuration), "token");
                        }
                    }
                );
                return output;
            }

            Result<MediaUri> stream_uri(const ProfileToken& profile) override
            {
                auto endpoint = service_endpoint(Service::media);
                if (!endpoint) return endpoint.error();
                const auto body =
                    "<trt:StreamSetup><tt:Stream>RTP-Unicast</tt:Stream>"
                    "<tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport>"
                    "</trt:StreamSetup><trt:ProfileToken>" +
                    xml_escape(profile) + "</trt:ProfileToken>";
                auto response = invoke(
                    endpoint.value(), OperationId::get_stream_uri, body, "GetStreamUriResponse"
                );
                if (!response) return response.error();
                return media_uri(response.value());
            }

            Result<MediaUri> snapshot_uri(const ProfileToken& profile) override
            {
                auto endpoint = service_endpoint(Service::media);
                if (!endpoint) return endpoint.error();
                auto response = invoke(
                    endpoint.value(),
                    OperationId::get_snapshot_uri,
                    "<trt:ProfileToken>" + xml_escape(profile) + "</trt:ProfileToken>",
                    "GetSnapshotUriResponse"
                );
                if (!response) return response.error();
                return media_uri(response.value());
            }

            Result<PtzSpaces> ptz_spaces(const ProfileToken& profile) override
            {
                {
                    std::lock_guard lock(profile_mutex_);
                    const auto cached = ptz_spaces_.find(profile);
                    if (cached != ptz_spaces_.end()) return cached->second;
                }
                auto endpoint = service_endpoint(Service::ptz);
                if (!endpoint) return endpoint.error();
                std::string configuration;
                {
                    std::lock_guard lock(profile_mutex_);
                    const auto found = ptz_configurations_.find(profile);
                    if (found != ptz_configurations_.end()) configuration = found->second;
                }
                if (configuration.empty())
                {
                    auto loaded = profiles();
                    if (!loaded) return loaded.error();
                    std::lock_guard lock(profile_mutex_);
                    const auto found = ptz_configurations_.find(profile);
                    if (found != ptz_configurations_.end()) configuration = found->second;
                }
                if (configuration.empty())
                {
                    return Error{
                        ErrorCategory::unsupported,
                        OperationId::get_ptz_configuration_options,
                        "profile has no PTZ configuration token"
                    };
                }
                auto response = invoke(
                    endpoint.value(),
                    OperationId::get_ptz_configuration_options,
                    "<tptz:ConfigurationToken>" + xml_escape(configuration) +
                        "</tptz:ConfigurationToken>",
                    "GetConfigurationOptionsResponse"
                );
                if (!response) return response.error();
                PtzSpaces output;
                const auto spaces = subtree(response.value(), "Spaces");
                const auto parse_2d = [&](std::string_view name, auto& destination)
                {
                    detail_xml::for_each_element(
                        spaces,
                        name,
                        [&](const detail_xml::Element& element)
                        {
                            const auto body = std::string(detail_xml::inner(spaces, element));
                            const auto x = first_range(body, "XRange");
                            const auto y = first_range(body, "YRange");
                            if (x && y)
                                destination.push_back({first_element(body, "URI"), *x, *y});
                        }
                    );
                };
                const auto parse_1d = [&](std::string_view name, auto& destination)
                {
                    detail_xml::for_each_element(
                        spaces,
                        name,
                        [&](const detail_xml::Element& element)
                        {
                            const auto body = std::string(detail_xml::inner(spaces, element));
                            const auto x = first_range(body, "XRange");
                            if (x) destination.push_back({first_element(body, "URI"), *x});
                        }
                    );
                };
                parse_2d("AbsolutePanTiltPositionSpace", output.absolute_pan_tilt);
                parse_1d("AbsoluteZoomPositionSpace", output.absolute_zoom);
                parse_2d("RelativePanTiltTranslationSpace", output.relative_pan_tilt);
                parse_1d("RelativeZoomTranslationSpace", output.relative_zoom);
                parse_2d("ContinuousPanTiltVelocitySpace", output.continuous_pan_tilt);
                parse_1d("ContinuousZoomVelocitySpace", output.continuous_zoom);
                const auto& pan_tilt = !output.continuous_pan_tilt.empty()
                                           ? output.continuous_pan_tilt.front()
                                           : !output.absolute_pan_tilt.empty()
                                                 ? output.absolute_pan_tilt.front()
                                                 : !output.relative_pan_tilt.empty()
                                                       ? output.relative_pan_tilt.front()
                                                       : PtzSpace2D{};
                const auto& zoom = !output.continuous_zoom.empty()
                                       ? output.continuous_zoom.front()
                                       : !output.absolute_zoom.empty()
                                             ? output.absolute_zoom.front()
                                             : !output.relative_zoom.empty()
                                                   ? output.relative_zoom.front()
                                                   : PtzSpace1D{};
                output.pan = pan_tilt.x;
                output.tilt = pan_tilt.y;
                output.zoom = zoom.x;
                {
                    std::lock_guard lock(profile_mutex_);
                    ptz_spaces_.insert_or_assign(profile, output);
                }
                return output;
            }

            Result<PtzStatus> ptz_status(const ProfileToken& profile) override
            {
                auto endpoint = service_endpoint(Service::ptz);
                if (!endpoint) return endpoint.error();
                auto response = invoke(
                    endpoint.value(),
                    OperationId::ptz_status,
                    "<tptz:ProfileToken>" + xml_escape(profile) + "</tptz:ProfileToken>",
                    "GetStatusResponse"
                );
                if (!response) return response.error();
                PtzStatus status;
                auto ranges = ptz_spaces(profile);
                if (!ranges) return ranges.error();
                const auto position_pan_tilt = ranges.value().absolute_pan_tilt.empty()
                                                   ? PtzSpace2D{"", ranges.value().pan, ranges.value().tilt}
                                                   : ranges.value().absolute_pan_tilt.front();
                const auto position_zoom = ranges.value().absolute_zoom.empty()
                                               ? PtzSpace1D{"", ranges.value().zoom}
                                               : ranges.value().absolute_zoom.front();
                bool position_seen = false;
                if (auto value = numeric_attribute(response.value(), "PanTilt", "x"))
                {
                    position_seen = true;
                    auto normalized = normalize_ptz(*value, position_pan_tilt.x);
                    if (normalized) status.position.pan = normalized.value();
                    else status.position_reliable = false;
                }
                if (auto value = numeric_attribute(response.value(), "PanTilt", "y"))
                {
                    position_seen = true;
                    auto normalized = normalize_ptz(*value, position_pan_tilt.y);
                    if (normalized) status.position.tilt = normalized.value();
                    else status.position_reliable = false;
                }
                if (auto value = numeric_attribute(response.value(), "Zoom", "x"))
                {
                    position_seen = true;
                    auto normalized = normalize_ptz(*value, position_zoom.x);
                    if (normalized) status.position.zoom = normalized.value();
                    else status.position_reliable = false;
                }
                const auto move = subtree(response.value(), "MoveStatus");
                const auto pan_tilt = first_element(move, "PanTilt");
                const auto zoom = first_element(move, "Zoom");
                if (!pan_tilt.empty()) status.pan_tilt_moving = pan_tilt == "MOVING";
                if (!zoom.empty()) status.zoom_moving = zoom == "MOVING";
                status.moving = status.pan_tilt_moving.value_or(false) ||
                                status.zoom_moving.value_or(false) ||
                                first_element(response.value(), "MoveStatus").find("MOVING") !=
                                    std::string::npos;
                if (!position_seen) status.position_reliable = false;
                return status;
            }

            Result<MoveResult>
            move(const ProfileToken& profile, PtzVector vector, bool relative) override
            {
                auto endpoint = service_endpoint(Service::ptz);
                if (!endpoint) return endpoint.error();
                auto ranges = ptz_spaces(profile);
                if (!ranges) return ranges.error();
                const auto pan_tilt_space = relative
                                                ? (ranges.value().relative_pan_tilt.empty()
                                                       ? PtzSpace2D{"", ranges.value().pan, ranges.value().tilt}
                                                       : ranges.value().relative_pan_tilt.front())
                                                : (ranges.value().continuous_pan_tilt.empty()
                                                       ? PtzSpace2D{"", ranges.value().pan, ranges.value().tilt}
                                                       : ranges.value().continuous_pan_tilt.front());
                const auto zoom_space = relative
                                            ? (ranges.value().relative_zoom.empty()
                                                   ? PtzSpace1D{"", ranges.value().zoom}
                                                   : ranges.value().relative_zoom.front())
                                            : (ranges.value().continuous_zoom.empty()
                                                   ? PtzSpace1D{"", ranges.value().zoom}
                                                   : ranges.value().continuous_zoom.front());
                const auto native = [](std::optional<double> value, NativeRange range)
                {
                    if (!value) return value;
                    auto converted = denormalize_ptz(*value, range);
                    return converted ? std::optional<double>(converted.value()) : std::nullopt;
                };
                vector.pan = native(vector.pan, pan_tilt_space.x);
                vector.tilt = native(vector.tilt, pan_tilt_space.y);
                vector.zoom = native(vector.zoom, zoom_space.x);
                // Stream formatting would emit the global locale's decimal separator
                // and truncate to six significant digits; XML doubles are neither.
                const auto& number = detail_xml::format_double;
                std::string body;
                body += "<tptz:ProfileToken>" + xml_escape(profile) + "</tptz:ProfileToken>";
                body += relative ? "<tptz:Translation>" : "<tptz:Velocity>";
                if (vector.pan || vector.tilt)
                {
                    body += "<tt:PanTilt";
                    if (!pan_tilt_space.uri.empty())
                        body += " space=\"" + xml_escape(pan_tilt_space.uri) + '"';
                    if (vector.pan) body += " x=\"" + number(*vector.pan) + '"';
                    if (vector.tilt) body += " y=\"" + number(*vector.tilt) + '"';
                    body += "/>";
                }
                if (vector.zoom)
                {
                    body += "<tt:Zoom";
                    if (!zoom_space.uri.empty())
                        body += " space=\"" + xml_escape(zoom_space.uri) + '"';
                    body += " x=\"" + number(*vector.zoom) + "\"/>";
                }
                body += relative ? "</tptz:Translation>" : "</tptz:Velocity>";
                const auto operation =
                    relative ? OperationId::relative_move : OperationId::continuous_move;
                auto response = invoke(
                    endpoint.value(),
                    operation,
                    body,
                    relative ? "RelativeMoveResponse" : "ContinuousMoveResponse"
                );
                if (!response)
                {
                    auto error = response.error();
                    error.request_may_have_been_applied = true;
                    return error;
                }
                return MoveResult{true, std::nullopt};
            }

            Result<MoveResult> absolute_move(
                const ProfileToken& profile,
                PtzVector position,
                std::optional<PtzVector> speed
            ) override
            {
                auto endpoint = service_endpoint(Service::ptz);
                if (!endpoint) return endpoint.error();
                auto ranges = ptz_spaces(profile);
                if (!ranges) return ranges.error();
                if (ranges.value().absolute_pan_tilt.empty() &&
                    ranges.value().absolute_zoom.empty())
                {
                    return Error{
                        ErrorCategory::unsupported,
                        OperationId::absolute_move,
                        "device advertises no absolute PTZ position space"
                    };
                }
                const auto pan_tilt = ranges.value().absolute_pan_tilt.empty()
                                          ? PtzSpace2D{"", ranges.value().pan, ranges.value().tilt}
                                          : ranges.value().absolute_pan_tilt.front();
                const auto zoom = ranges.value().absolute_zoom.empty()
                                      ? PtzSpace1D{"", ranges.value().zoom}
                                      : ranges.value().absolute_zoom.front();
                const auto velocity_pan_tilt = ranges.value().continuous_pan_tilt.empty()
                                                   ? PtzSpace2D{"", ranges.value().pan, ranges.value().tilt}
                                                   : ranges.value().continuous_pan_tilt.front();
                const auto velocity_zoom = ranges.value().continuous_zoom.empty()
                                               ? PtzSpace1D{"", ranges.value().zoom}
                                               : ranges.value().continuous_zoom.front();
                const auto native = [](std::optional<double> value, NativeRange range)
                {
                    if (!value) return value;
                    auto converted = denormalize_ptz(*value, range);
                    return converted ? std::optional<double>(converted.value()) : std::nullopt;
                };
                position.pan = native(position.pan, pan_tilt.x);
                position.tilt = native(position.tilt, pan_tilt.y);
                position.zoom = native(position.zoom, zoom.x);
                if (speed)
                {
                    speed->pan = native(speed->pan, velocity_pan_tilt.x);
                    speed->tilt = native(speed->tilt, velocity_pan_tilt.y);
                    speed->zoom = native(speed->zoom, velocity_zoom.x);
                }
                const auto append = [&](std::string& body, const PtzVector& vector, bool velocity)
                {
                    const auto& number = detail_xml::format_double;
                    if (vector.pan || vector.tilt)
                    {
                        body += "<tt:PanTilt";
                        const auto& uri = velocity ? velocity_pan_tilt.uri : pan_tilt.uri;
                        if (!uri.empty()) body += " space=\"" + xml_escape(uri) + '"';
                        if (vector.pan) body += " x=\"" + number(*vector.pan) + '"';
                        if (vector.tilt) body += " y=\"" + number(*vector.tilt) + '"';
                        body += "/>";
                    }
                    if (vector.zoom)
                    {
                        body += "<tt:Zoom";
                        const auto& uri = velocity ? velocity_zoom.uri : zoom.uri;
                        if (!uri.empty()) body += " space=\"" + xml_escape(uri) + '"';
                        body += " x=\"" + number(*vector.zoom) + "\"/>";
                    }
                };
                std::string body = "<tptz:ProfileToken>" + xml_escape(profile) +
                                   "</tptz:ProfileToken><tptz:Position>";
                append(body, position, false);
                body += "</tptz:Position>";
                if (speed)
                {
                    body += "<tptz:Speed>";
                    append(body, *speed, true);
                    body += "</tptz:Speed>";
                }
                auto response = invoke(
                    endpoint.value(), OperationId::absolute_move, body, "AbsoluteMoveResponse"
                );
                if (!response)
                {
                    auto error = response.error();
                    error.request_may_have_been_applied = true;
                    return error;
                }
                return MoveResult{true, std::nullopt};
            }

            Result<void> stop(const ProfileToken& profile) override
            {
                auto endpoint = service_endpoint(Service::ptz);
                if (!endpoint) return endpoint.error();
                auto response = invoke(
                    endpoint.value(),
                    OperationId::ptz_stop,
                    "<tptz:ProfileToken>" + xml_escape(profile) +
                        "</tptz:ProfileToken><tptz:PanTilt>true</tptz:PanTilt>"
                        "<tptz:Zoom>true</tptz:Zoom>",
                    "StopResponse"
                );
                return response ? Result<void>{} : Result<void>{response.error()};
            }

            Result<std::vector<Preset>> presets(const ProfileToken& profile) override
            {
                auto endpoint = service_endpoint(Service::ptz);
                if (!endpoint) return endpoint.error();
                auto response = invoke(
                    endpoint.value(),
                    OperationId::get_presets,
                    "<tptz:ProfileToken>" + xml_escape(profile) + "</tptz:ProfileToken>",
                    "GetPresetsResponse"
                );
                if (!response) return response.error();
                // Preset positions are advertised in the device's native units, but
                // PtzVector is defined as normalized [-1, +1]. Publishing the raw
                // values would make a preset position unusable as a move argument.
                std::optional<PtzSpaces> ranges;
                if (auto spaces = ptz_spaces(profile)) ranges = spaces.value();
                const auto normalized =
                    [&](std::optional<double> value, NativeRange range) -> std::optional<double>
                {
                    if (!ranges || !value) return std::nullopt;
                    auto converted = normalize_ptz(*value, range);
                    return converted ? std::optional<double>(converted.value()) : std::nullopt;
                };
                std::vector<Preset> output;
                const std::string& xml = response.value();
                detail_xml::for_each_element(
                    xml,
                    "Preset",
                    [&](const detail_xml::Element& element)
                    {
                        Preset preset;
                        preset.token = attribute(detail_xml::attributes(xml, element), "token");
                        const auto body = std::string(detail_xml::inner(xml, element));
                        preset.name = first_element(body, "Name");
                        const auto pan_tilt = ranges ? ranges->pan : NativeRange{};
                        const auto tilt = ranges ? ranges->tilt : NativeRange{};
                        const auto zoom = ranges ? ranges->zoom : NativeRange{};
                        preset.position.pan =
                            normalized(numeric_attribute(body, "PanTilt", "x"), pan_tilt);
                        preset.position.tilt =
                            normalized(numeric_attribute(body, "PanTilt", "y"), tilt);
                        preset.position.zoom =
                            normalized(numeric_attribute(body, "Zoom", "x"), zoom);
                        output.push_back(std::move(preset));
                    }
                );
                return output;
            }

            Result<void>
            goto_preset(const ProfileToken& profile, const PresetToken& preset) override
            {
                auto endpoint = service_endpoint(Service::ptz);
                if (!endpoint) return endpoint.error();
                auto response = invoke(
                    endpoint.value(),
                    OperationId::goto_preset,
                    "<tptz:ProfileToken>" + xml_escape(profile) +
                        "</tptz:ProfileToken><tptz:PresetToken>" + xml_escape(preset) +
                        "</tptz:PresetToken>",
                    "GotoPresetResponse"
                );
                return response ? Result<void>{} : Result<void>{response.error()};
            }

            Result<std::vector<Event>> pull_events(std::size_t maximum) override
            {
                std::lock_guard lock(event_mutex_);
                if (subscription_url_.empty())
                {
                    auto endpoint = service_endpoint(Service::events);
                    if (!endpoint) return endpoint.error();
                    auto subscribed = invoke(
                        endpoint.value(),
                        OperationId::event_subscribe,
                        "<tev:InitialTerminationTime>PT60S</tev:InitialTerminationTime>",
                        "CreatePullPointSubscriptionResponse"
                    );
                    if (!subscribed) return subscribed.error();
                    // The address must come from the SubscriptionReference; the
                    // envelope also carries wsa:Address elements in its header.
                    const auto reference = subtree(subscribed.value(), "SubscriptionReference");
                    subscription_url_ = first_element(reference, "Address");
                    if (subscription_url_.empty())
                    {
                        return Error{
                            ErrorCategory::protocol_violation,
                            OperationId::event_subscribe,
                            "subscription endpoint missing"
                        };
                    }
                }
                auto response = invoke(
                    subscription_url_,
                    OperationId::event_pull,
                    "<tev:Timeout>PT5S</tev:Timeout><tev:MessageLimit>" +
                        std::to_string(maximum) + "</tev:MessageLimit>",
                    "PullMessagesResponse"
                );
                if (!response)
                {
                    subscription_url_.clear();
                    return response.error();
                }
                std::vector<Event> output;
                const std::string& xml = response.value();
                detail_xml::for_each_element(
                    xml,
                    "NotificationMessage",
                    [&](const detail_xml::Element& element)
                    {
                        Event event;
                        const auto body = std::string(detail_xml::inner(xml, element));
                        event.topic = first_element(body, "Topic");
                        // ONVIF puts SimpleItems under both Source and Data. Taking
                        // the first item as the source and lumping the rest into data
                        // misfiles every message with more than one source item.
                        const auto message = detail_xml::find_element(body, "Message");
                        const auto payload = message
                                                 ? std::string(detail_xml::inner(body, *message))
                                                 : body;
                        const auto source = detail_xml::find_element(payload, "Source");
                        const auto data = detail_xml::find_element(payload, "Data");
                        if (source || data)
                        {
                            if (source)
                            {
                                event.source =
                                    simple_items(detail_xml::inner(payload, *source));
                            }
                            if (data)
                            {
                                event.data = simple_items(detail_xml::inner(payload, *data));
                            }
                        }
                        else
                        {
                            event.data = simple_items(payload);
                        }
                        // Message carries the device's own UtcTime; falling back to
                        // the local clock only when the device omits or mangles it.
                        std::string stamp;
                        if (message)
                        {
                            const auto inner_message =
                                detail_xml::find_element(payload, "Message");
                            stamp = attribute(
                                inner_message
                                    ? detail_xml::attributes(payload, *inner_message)
                                    : detail_xml::attributes(body, *message),
                                "UtcTime"
                            );
                        }
                        const auto parsed = parse_xsd_datetime(stamp);
                        event.utc_time =
                            parsed ? *parsed : std::chrono::system_clock::now();
                        event.sequence = next_sequence_++;
                        output.push_back(std::move(event));
                    }
                );
                return output;
            }

            Result<void> renew_events() override
            {
                std::lock_guard lock(event_mutex_);
                if (subscription_url_.empty()) return {};
                auto response = invoke(
                    subscription_url_,
                    OperationId::event_renew,
                    "<wsnt:TerminationTime>PT60S</wsnt:TerminationTime>",
                    "RenewResponse"
                );
                if (!response)
                {
                    subscription_url_.clear();
                    return response.error();
                }
                return {};
            }

            Result<void> close_events() override
            {
                std::lock_guard lock(event_mutex_);
                if (subscription_url_.empty()) return {};
                auto response = invoke(
                    subscription_url_, OperationId::event_close, {}, "UnsubscribeResponse"
                );
                subscription_url_.clear();
                return response ? Result<void>{} : Result<void>{response.error()};
            }

          private:
            enum class Service { media, ptz, events };

            Result<std::string> service_endpoint(Service service)
            {
                // GetCapabilities must not run while the lock is held. A safety Stop
                // would otherwise queue behind whatever request is already in flight,
                // for the full duration of that request's timeout. Racing callers may
                // each issue one GetCapabilities; the results are identical.
                bool loaded = false;
                {
                    std::lock_guard lock(capabilities_mutex_);
                    loaded = capabilities_loaded_;
                }
                if (!loaded)
                {
                    auto response = invoke(
                        device_url_,
                        OperationId::get_capabilities,
                        {},
                        "GetCapabilitiesResponse"
                    );
                    if (!response) return response.error();
                    auto media = endpoint_in(response.value(), "Media");
                    auto ptz = endpoint_in(response.value(), "PTZ");
                    auto events = endpoint_in(response.value(), "Events");
                    std::lock_guard lock(capabilities_mutex_);
                    if (!capabilities_loaded_)
                    {
                        media_url_ = std::move(media);
                        ptz_url_ = std::move(ptz);
                        event_url_ = std::move(events);
                        capabilities_loaded_ = true;
                    }
                }
                std::unique_lock lock(capabilities_mutex_);
                const auto endpoint = service == Service::media ? media_url_
                                      : service == Service::ptz ? ptz_url_
                                                                : event_url_;
                lock.unlock();
                if (endpoint.empty())
                {
                    return Error{
                        ErrorCategory::unsupported, {}, "device did not advertise the service"
                    };
                }
                auto authorized = policy_->authorize(endpoint, device_url_);
                return authorized ? Result<std::string>(endpoint)
                                  : Result<std::string>(authorized.error());
            }

            Result<std::string> invoke(
                const std::string& endpoint,
                OperationId operation,
                std::string body,
                std::string_view response_name
            )
            {
                auto authorized = policy_->authorize(endpoint, device_url_);
                if (!authorized) return authorized.error();
                HttpRequest request{
                    endpoint,
                    soap_envelope(operation, body, credentials_),
                    {"Content-Type: application/soap+xml; charset=utf-8"},
                    active_call_options
                        ? active_call_options->deadline
                        : std::chrono::steady_clock::now() + std::chrono::seconds(15),
                    credentials_
                };
                auto response = wait_for_task(
                    transport_->send(
                        std::move(request),
                        active_call_options ? active_call_options->cancellation
                                            : CancellationSource{}.token()
                    )
                );
                if (!response) return response.error();
                if (response.value().status == 401)
                    return Error{ErrorCategory::authentication, operation, "authentication failed"};
                if (response.value().status == 403)
                    return Error{ErrorCategory::authorization, operation, "request was forbidden"};
                auto parsed = read_soap_operation(response.value().body, response_name);
                if (!parsed)
                {
                    auto error = parsed.error();
                    error.operation = operation;
                    return error;
                }
                return std::move(response.value().body);
            }

            static std::string endpoint_in(std::string_view xml, std::string_view service)
            {
                const auto element = detail_xml::find_element(xml, service);
                return element ? first_element(detail_xml::inner(xml, *element), "XAddr")
                               : std::string{};
            }

            static MediaUri media_uri(const std::string& xml)
            {
                MediaUri output;
                output.uri = first_element(xml, "Uri");
                output.invalid_after_connect = first_element(xml, "InvalidAfterConnect") == "true";
                return output;
            }

            std::string device_url_;
            std::shared_ptr<HttpTransport> transport_;
            std::shared_ptr<EndpointPolicy> policy_;
            std::shared_ptr<Runtime> runtime_;
            std::optional<Credentials> credentials_;
            std::mutex capabilities_mutex_;
            bool capabilities_loaded_{};
            std::string media_url_;
            std::string ptz_url_;
            std::string event_url_;
            std::mutex event_mutex_;
            std::string subscription_url_;
            std::uint64_t next_sequence_{};
            std::mutex profile_mutex_;
            std::map<ProfileToken, std::string> ptz_configurations_;
            std::map<ProfileToken, PtzSpaces> ptz_spaces_;
        };
    } // namespace

    std::shared_ptr<ServiceBackend> make_network_service_backend(
        std::string device_url,
        std::shared_ptr<HttpTransport> transport,
        std::shared_ptr<EndpointPolicy> endpoint_policy,
        std::shared_ptr<Runtime> runtime,
        std::optional<Credentials> credentials
    )
    {
        return std::make_shared<NetworkServiceBackend>(
            std::move(device_url),
            std::move(transport),
            std::move(endpoint_policy),
            std::move(runtime),
            std::move(credentials)
        );
    }
} // namespace wlonvif

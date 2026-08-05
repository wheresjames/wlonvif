#include "network_services.hpp"

#include <condition_variable>
#include <map>
#include <regex>
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
        template <class T> T wait_for_task(Task<T> task)
        {
            std::mutex mutex;
            std::condition_variable ready;
            std::optional<T> result;
            std::move(task).start(
                [&](T value)
                {
                    {
                        std::lock_guard lock(mutex);
                        result.emplace(std::move(value));
                    }
                    ready.notify_one();
                }
            );
            std::unique_lock lock(mutex);
            ready.wait(lock, [&] { return result.has_value(); });
            return std::move(*result);
        }

        std::string xml_escape(std::string_view input)
        {
            std::string output;
            for (char value : input)
            {
                switch (value)
                {
                    case '&': output += "&amp;"; break;
                    case '<': output += "&lt;"; break;
                    case '>': output += "&gt;"; break;
                    case '"': output += "&quot;"; break;
                    case '\'': output += "&apos;"; break;
                    default: output += value;
                }
            }
            return output;
        }

        std::string xml_unescape(std::string value)
        {
            const std::pair<std::string_view, std::string_view> entities[]{
                {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""},
                {"&apos;", "'"}
            };
            for (const auto& [encoded, decoded] : entities)
            {
                std::size_t position = 0;
                while ((position = value.find(encoded, position)) != value.npos)
                {
                    value.replace(position, encoded.size(), decoded);
                    position += decoded.size();
                }
            }
            return value;
        }

        std::string first_element(std::string_view xml, std::string_view local_name)
        {
            const std::regex expression(
                "<(?:[A-Za-z_][A-Za-z0-9_.-]*:)?" + std::string(local_name) +
                    "(?:\\s[^>]*)?>([\\s\\S]*?)</(?:[A-Za-z_][A-Za-z0-9_.-]*:)?" +
                    std::string(local_name) + ">",
                std::regex::icase
            );
            std::match_results<std::string_view::const_iterator> match;
            return std::regex_search(xml.begin(), xml.end(), match, expression)
                       ? xml_unescape(std::string(match[1].first, match[1].second))
                       : std::string{};
        }

        std::string attribute(std::string_view tag, std::string_view name)
        {
            const std::regex expression(
                std::string(name) + "\\s*=\\s*['\"]([^'\"]*)['\"]", std::regex::icase
            );
            std::match_results<std::string_view::const_iterator> match;
            return std::regex_search(tag.begin(), tag.end(), match, expression)
                       ? xml_unescape(std::string(match[1].first, match[1].second))
                       : std::string{};
        }

        std::optional<double> numeric_attribute(
            std::string_view xml, std::string_view element, std::string_view name
        )
        {
            const std::regex expression(
                "<(?:[A-Za-z_][A-Za-z0-9_.-]*:)?" + std::string(element) +
                    "\\b[^>]*\\b" + std::string(name) + "\\s*=\\s*['\"]([^'\"]+)['\"]",
                std::regex::icase
            );
            std::match_results<std::string_view::const_iterator> match;
            if (!std::regex_search(xml.begin(), xml.end(), match, expression))
            {
                return std::nullopt;
            }
            try
            {
                return std::stod(std::string(match[1].first, match[1].second));
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        std::optional<NativeRange> first_range(std::string_view xml, std::string_view name)
        {
            const auto section = first_element(xml, name);
            if (section.empty()) return std::nullopt;
            const auto minimum = first_element(section, "Min");
            const auto maximum = first_element(section, "Max");
            try
            {
                if (!minimum.empty() && !maximum.empty())
                    return NativeRange{std::stod(minimum), std::stod(maximum)};
            }
            catch (...)
            {
            }
            return std::nullopt;
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
                const std::regex expression(
                    "<(?:[A-Za-z_][A-Za-z0-9_.-]*:)?Profiles\\b([^>]*)>([\\s\\S]*?)"
                    "</(?:[A-Za-z_][A-Za-z0-9_.-]*:)?Profiles>",
                    std::regex::icase
                );
                for (std::sregex_iterator current(
                         response.value().begin(), response.value().end(), expression
                     ), end;
                     current != end;
                     ++current)
                {
                    const auto token = attribute((*current)[1].str(), "token");
                    output.push_back({token, first_element((*current)[2].str(), "Name")});
                    const std::regex ptz_configuration(
                        "<(?:[A-Za-z_][A-Za-z0-9_.-]*:)?PTZConfiguration\\b([^>]*)>",
                        std::regex::icase
                    );
                    std::smatch configuration;
                    const auto profile_body = (*current)[2].str();
                    if (std::regex_search(profile_body, configuration, ptz_configuration))
                    {
                        std::lock_guard lock(profile_mutex_);
                        ptz_configurations_[token] = attribute(configuration[1].str(), "token");
                    }
                }
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
                const auto pan_tilt = first_element(
                    response.value(), "ContinuousPanTiltVelocitySpace"
                );
                const auto zoom = first_element(response.value(), "ContinuousZoomVelocitySpace");
                if (auto range = first_range(pan_tilt, "XRange")) output.pan = *range;
                if (auto range = first_range(pan_tilt, "YRange")) output.tilt = *range;
                if (auto range = first_range(zoom, "XRange")) output.zoom = *range;
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
                if (auto value = numeric_attribute(response.value(), "PanTilt", "x"))
                {
                    auto normalized = normalize_ptz(*value, ranges.value().pan);
                    if (normalized) status.position.pan = normalized.value();
                }
                if (auto value = numeric_attribute(response.value(), "PanTilt", "y"))
                {
                    auto normalized = normalize_ptz(*value, ranges.value().tilt);
                    if (normalized) status.position.tilt = normalized.value();
                }
                if (auto value = numeric_attribute(response.value(), "Zoom", "x"))
                {
                    auto normalized = normalize_ptz(*value, ranges.value().zoom);
                    if (normalized) status.position.zoom = normalized.value();
                }
                const auto move = first_element(response.value(), "MoveStatus");
                status.moving = move.find("MOVING") != std::string::npos;
                return status;
            }

            Result<MoveResult>
            move(const ProfileToken& profile, PtzVector vector, bool relative) override
            {
                auto endpoint = service_endpoint(Service::ptz);
                if (!endpoint) return endpoint.error();
                auto ranges = ptz_spaces(profile);
                if (!ranges) return ranges.error();
                const auto native = [](std::optional<double> value, NativeRange range)
                {
                    if (!value) return value;
                    auto converted = denormalize_ptz(*value, range);
                    return converted ? std::optional<double>(converted.value()) : std::nullopt;
                };
                vector.pan = native(vector.pan, ranges.value().pan);
                vector.tilt = native(vector.tilt, ranges.value().tilt);
                vector.zoom = native(vector.zoom, ranges.value().zoom);
                std::ostringstream body;
                body << "<tptz:ProfileToken>" << xml_escape(profile) << "</tptz:ProfileToken>";
                body << (relative ? "<tptz:Translation>" : "<tptz:Velocity>");
                if (vector.pan || vector.tilt)
                {
                    body << "<tt:PanTilt";
                    if (vector.pan) body << " x=\"" << *vector.pan << '"';
                    if (vector.tilt) body << " y=\"" << *vector.tilt << '"';
                    body << "/>";
                }
                if (vector.zoom) body << "<tt:Zoom x=\"" << *vector.zoom << "\"/>";
                body << (relative ? "</tptz:Translation>" : "</tptz:Velocity>");
                const auto operation =
                    relative ? OperationId::relative_move : OperationId::continuous_move;
                auto response = invoke(
                    endpoint.value(),
                    operation,
                    body.str(),
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
                std::vector<Preset> output;
                const std::regex expression(
                    "<(?:[A-Za-z_][A-Za-z0-9_.-]*:)?Preset\\b([^>]*)>([\\s\\S]*?)"
                    "</(?:[A-Za-z_][A-Za-z0-9_.-]*:)?Preset>",
                    std::regex::icase
                );
                for (std::sregex_iterator current(
                         response.value().begin(), response.value().end(), expression
                     ), end;
                     current != end;
                     ++current)
                {
                    Preset preset;
                    preset.token = attribute((*current)[1].str(), "token");
                    preset.name = first_element((*current)[2].str(), "Name");
                    preset.position.pan = numeric_attribute((*current)[2].str(), "PanTilt", "x");
                    preset.position.tilt = numeric_attribute((*current)[2].str(), "PanTilt", "y");
                    preset.position.zoom = numeric_attribute((*current)[2].str(), "Zoom", "x");
                    output.push_back(std::move(preset));
                }
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
                    subscription_url_ = first_element(subscribed.value(), "Address");
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
                const std::regex notification(
                    "<(?:[A-Za-z_][A-Za-z0-9_.-]*:)?NotificationMessage\\b[^>]*>"
                    "([\\s\\S]*?)</(?:[A-Za-z_][A-Za-z0-9_.-]*:)?NotificationMessage>",
                    std::regex::icase
                );
                for (std::sregex_iterator current(
                         response.value().begin(), response.value().end(), notification
                     ), end;
                     current != end;
                     ++current)
                {
                    Event event;
                    event.topic = first_element((*current)[1].str(), "Topic");
                    event.utc_time = std::chrono::system_clock::now();
                    const std::regex item(
                        "<(?:[A-Za-z_][A-Za-z0-9_.-]*:)?SimpleItem\\b([^>]*)/?>",
                        std::regex::icase
                    );
                    for (std::sregex_iterator value(
                             (*current)[1].first, (*current)[1].second, item
                         );
                         value != std::sregex_iterator{};
                         ++value)
                    {
                        const auto name = attribute((*value)[1].str(), "Name");
                        const auto data = attribute((*value)[1].str(), "Value");
                        if (event.source.empty()) event.source = name + '=' + data;
                        else event.data += (event.data.empty() ? "" : ",") + name + '=' + data;
                    }
                    event.sequence = next_sequence_++;
                    output.push_back(std::move(event));
                }
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
                std::lock_guard lock(capabilities_mutex_);
                if (!capabilities_loaded_)
                {
                    auto response = invoke(
                        device_url_,
                        OperationId::get_capabilities,
                        {},
                        "GetCapabilitiesResponse"
                    );
                    if (!response) return response.error();
                    media_url_ = endpoint_in(response.value(), "Media");
                    ptz_url_ = endpoint_in(response.value(), "PTZ");
                    event_url_ = endpoint_in(response.value(), "Events");
                    capabilities_loaded_ = true;
                }
                const auto& endpoint = service == Service::media   ? media_url_
                                       : service == Service::ptz ? ptz_url_
                                                                  : event_url_;
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
                const std::regex expression(
                    "<(?:[A-Za-z_][A-Za-z0-9_.-]*:)?" + std::string(service) +
                        "\\b[^>]*>([\\s\\S]*?)</(?:[A-Za-z_][A-Za-z0-9_.-]*:)?" +
                        std::string(service) + ">",
                    std::regex::icase
                );
                std::match_results<std::string_view::const_iterator> match;
                return std::regex_search(xml.begin(), xml.end(), match, expression)
                           ? first_element(
                                 std::string_view(match[1].first, match[1].second), "XAddr"
                             )
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

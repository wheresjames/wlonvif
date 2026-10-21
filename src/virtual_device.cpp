#include "dispatch.hpp"
#include "soap_response.hpp"
#include "xml_scan.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <deque>
#include <future>
#include <map>
#include <wlonvif/wlonvif.hpp>

namespace wlonvif
{
    struct VirtualDeviceTransport::VirtualServiceState
    {
        std::mutex mutex;
        struct ProfileState
        {
            MediaProfile profile;
            MediaUri stream, snapshot;
            PtzSpaces spaces;
            PtzStatus status;
            std::vector<Preset> presets;
        };
        std::map<ProfileToken, ProfileState> profiles;
        std::deque<Event> events;
        std::optional<Error> next_move_error;
        std::optional<Error> stop_error;
        std::map<ProfileToken, std::size_t> stop_attempts;
        std::chrono::milliseconds stop_delay{};

        // Device configuration changed through the Device service.
        struct ScopeState
        {
            std::string uri;
            bool fixed{};
        };
        std::vector<ScopeState> scopes;
        DateTimeType clock_type{DateTimeType::ntp};
        bool daylight_savings{};
        std::string time_zone{"UTC0"};
        std::chrono::system_clock::duration clock_offset{};
        NtpConfiguration ntp{true, {}, {}};

        std::vector<EventTopic> topics;
        std::map<std::string, std::vector<std::string>> subscription_topics;
        std::uint64_t next_subscription{1};
        struct Analytics
        {
            std::vector<AnalyticsConfig> modules, rules;
        };
        std::map<std::string, Analytics> analytics;
        struct Imaging
        {
            VideoSource source;
            ImagingSettings settings;
            ImagingOptions options;
        };
        std::vector<Imaging> imaging;
        std::vector<ProfileConfiguration> profile_configurations;
        std::map<std::string, VideoEncoderOptions> encoder_options;
        std::map<ProfileToken, std::vector<PresetTour>> tours;
        std::map<ProfileToken, PtzConfiguration> ptz_configurations;
        std::uint64_t next_token{1};
        std::string hostname{"virtual-camera"};
        bool discoverable{true};
        std::size_t reboots{};
    };
    namespace
    {
        std::string esc(std::string_view s)
        {
            return detail_xml::escape(s);
        }
        std::string tag(std::string_view n, std::string_view v, std::string_view p = "tds")
        {
            return "<" + std::string(p) + ":" + std::string(n) + ">" + esc(v) + "</" +
                   std::string(p) + ":" + std::string(n) + ">";
        }
        /// Serialize a UTC time the way a Device service reports it.
        std::string utc_date_and_time(std::chrono::system_clock::time_point time)
        {
            const auto now = std::chrono::system_clock::to_time_t(time);
            std::tm utc{};
#ifdef _WIN32
            gmtime_s(&utc, &now);
#else
            gmtime_r(&now, &utc);
#endif
            const auto number = [](int value) { return std::to_string(value); };
            return "<tt:UTCDateTime><tt:Time>" + tag("Hour", number(utc.tm_hour), "tt") +
                   tag("Minute", number(utc.tm_min), "tt") +
                   tag("Second", number(utc.tm_sec), "tt") + "</tt:Time><tt:Date>" +
                   tag("Year", number(utc.tm_year + 1900), "tt") +
                   tag("Month", number(utc.tm_mon + 1), "tt") +
                   tag("Day", number(utc.tm_mday), "tt") + "</tt:Date></tt:UTCDateTime>";
        }
    } // namespace
    VirtualDeviceTransport::VirtualDeviceTransport(
        std::shared_ptr<Runtime> r,
        DeviceInformation i,
        std::vector<std::string> s,
        std::vector<Service> sv,
        Capabilities c
    )
        : runtime_(std::move(r)), info_(std::move(i)), scopes_(std::move(s)),
          services_(std::move(sv)), capabilities_(std::move(c)),
          service_state_(std::make_shared<VirtualServiceState>())
    {
        // Scopes supplied at construction are the device's built-in ones.
        for (const auto& scope : scopes_) service_state_->scopes.push_back({scope, true});
    }
    Task<Result<HttpResponse>>
    VirtualDeviceTransport::send(HttpRequest request, CancellationToken token)
    {
        auto rt = runtime_;
        auto info = info_;
        auto services = services_;
        auto caps = capabilities_;
        auto state = service_state_;
        return Task<Result<HttpResponse>>(
            [rt,
             state,
             request = std::move(request),
             token,
             info = std::move(info),
             services = std::move(services),
             caps = std::move(caps)](auto done) mutable
            {
                detail::dispatch(
                    rt,
                    [state,
                     request = std::move(request),
                     token,
                     info = std::move(info),
                     services = std::move(services),
                     caps = std::move(caps),
                     done = std::move(done)]() mutable
                    {
                        if (token.stop_requested())
                        {
                            done(Error{ErrorCategory::cancelled});
                            return;
                        }
                        std::string name, body;
                        const auto has = [&](std::string_view operation)
                        {
                            return request.body.find(operation) != request.body.npos;
                        };
                        std::unique_lock lock(state->mutex, std::defer_lock);
                        const auto fault = [&](std::string_view code, std::string_view subcode)
                        {
                            // Completion may issue another request; never hold the state lock.
                            if (lock.owns_lock()) lock.unlock();
                            done(HttpResponse{
                                400,
                                "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
                                "xmlns:ter=\"http://www.onvif.org/ver10/error\"><s:Body><s:Fault>"
                                "<s:Code><s:Value>s:Sender</s:Value><s:Subcode><s:Value>" +
                                    std::string(code) + "</s:Value><s:Subcode><s:Value>" +
                                    std::string(subcode) +
                                    "</s:Value></s:Subcode></s:Subcode></s:Code></s:Fault>"
                                    "</s:Body></s:Envelope>",
                                "application/soap+xml"
                            });
                        };
                        std::vector<std::string> items;
                        for (const auto element : {"ScopeItem", "Scopes"})
                        {
                            detail_xml::for_each_element(
                                request.body,
                                element,
                                [&](const detail_xml::Element& item)
                                { items.push_back(detail_xml::text(request.body, item)); }
                            );
                        }
                        lock.lock();
                        if (has("SetSystemDateAndTime"))
                        {
                            const auto& xml = request.body;
                            const bool manual = detail_xml::first_text(xml, "DateTimeType") == "Manual";
                            std::optional<std::chrono::system_clock::time_point> utc;
                            if (detail_xml::find_element(xml, "UTCDateTime"))
                            {
                                const auto number = [&](std::string_view field)
                                {
                                    return std::atoi(detail_xml::first_text(xml, field).c_str());
                                };
                                const auto date = std::chrono::year(number("Year")) /
                                                  std::chrono::month(static_cast<unsigned>(number("Month"))) /
                                                  std::chrono::day(static_cast<unsigned>(number("Day")));
                                if (!date.ok()) return fault("ter:InvalidArgVal", "ter:InvalidDateTime");
                                utc = std::chrono::sys_days(date) + std::chrono::hours(number("Hour")) +
                                      std::chrono::minutes(number("Minute")) +
                                      std::chrono::seconds(number("Second"));
                            }
                            if (manual && !utc) return fault("ter:InvalidArgVal", "ter:InvalidDateTime");
                            auto zone = detail_xml::first_text(xml, "TZ");
                            if (detail_xml::find_element(xml, "TZ") && zone.empty())
                                return fault("ter:InvalidArgVal", "ter:InvalidTimeZone");
                            state->clock_type = manual ? DateTimeType::manual : DateTimeType::ntp;
                            state->daylight_savings =
                                detail_xml::first_text(xml, "DaylightSavings") == "true";
                            if (!zone.empty()) state->time_zone = std::move(zone);
                            state->clock_offset = manual
                                ? *utc - std::chrono::system_clock::now()
                                : std::chrono::system_clock::duration{};
                            name = "SetSystemDateAndTime";
                        }
                        else if (has("GetHostname"))
                        {
                            name = "GetHostname";
                            body = "<tds:HostnameInformation>" + tag("FromDHCP", "false", "tt") +
                                   tag("Name", state->hostname, "tt") + "</tds:HostnameInformation>";
                        }
                        else if (has("SetHostname"))
                        {
                            auto hostname = detail_xml::first_text(request.body, "Name");
                            if (hostname.empty()) return fault("ter:InvalidArgVal", "ter:InvalidHostname");
                            state->hostname = std::move(hostname);
                            name = "SetHostname";
                        }
                        else if (has("GetNetworkInterfaces"))
                        {
                            name = "GetNetworkInterfaces";
                            body = "<tds:NetworkInterfaces token=\"eth0\">" + tag("Enabled", "true", "tt") +
                                   "<tt:Info>" + tag("Name", "eth0", "tt") +
                                   tag("HwAddress", "02-00-00-00-00-01", "tt") + tag("MTU", "1500", "tt") +
                                   "</tt:Info><tt:IPv4>" + tag("Enabled", "true", "tt") +
                                   "<tt:Config><tt:Manual>" + tag("Address", "192.0.2.10", "tt") +
                                   tag("PrefixLength", "24", "tt") + "</tt:Manual>" + tag("DHCP", "false", "tt") +
                                   "</tt:Config></tt:IPv4></tds:NetworkInterfaces>";
                        }
                        else if (has("GetDNS"))
                        {
                            name = "GetDNS";
                            body = "<tds:DNSInformation>" + tag("FromDHCP", "false", "tt") +
                                   "<tt:DNSManual>" + tag("Type", "IPv4", "tt") +
                                   tag("IPv4Address", "192.0.2.1", "tt") + "</tt:DNSManual></tds:DNSInformation>";
                        }
                        else if (has("GetNetworkDefaultGateway"))
                        {
                            name = "GetNetworkDefaultGateway";
                            body = "<tds:NetworkGateway>" + tag("IPv4Address", "192.0.2.1", "tt") +
                                   "</tds:NetworkGateway>";
                        }
                        else if (has("GetNetworkProtocols"))
                        {
                            name = "GetNetworkProtocols";
                            body = "<tds:NetworkProtocols>" + tag("Name", "HTTP", "tt") + tag("Enabled", "true", "tt") +
                                   tag("Port", "80", "tt") + "</tds:NetworkProtocols><tds:NetworkProtocols>" +
                                   tag("Name", "RTSP", "tt") + tag("Enabled", "true", "tt") + tag("Port", "554", "tt") +
                                   "</tds:NetworkProtocols>";
                        }
                        else if (has("GetDiscoveryMode"))
                        {
                            name = "GetDiscoveryMode";
                            body = tag("DiscoveryMode", state->discoverable ? "Discoverable" : "NonDiscoverable");
                        }
                        else if (has("SetDiscoveryMode"))
                        {
                            const auto mode = detail_xml::first_text(request.body, "DiscoveryMode");
                            if (mode != "Discoverable" && mode != "NonDiscoverable")
                                return fault("ter:InvalidArgVal", "ter:InvalidArgVal");
                            state->discoverable = mode == "Discoverable";
                            name = "SetDiscoveryMode";
                        }
                        else if (has("GetUsers"))
                        {
                            name = "GetUsers";
                            body = "<tds:User>" + tag("Username", "admin", "tt") +
                                   tag("UserLevel", "Administrator", "tt") + "</tds:User>";
                        }
                        else if (has("SystemReboot"))
                        {
                            ++state->reboots;
                            name = "SystemReboot";
                            body = tag("Message", "Rebooting");
                        }
                        else if (has("GetNTP"))
                        {
                            name = "GetNTP";
                            body = tag("FromDHCP", state->ntp.from_dhcp ? "true" : "false");
                            for (const auto& server : state->ntp.servers)
                                body += "<tds:NTPManual><tt:Type>DNS</tt:Type>" +
                                        tag("DNSname", server, "tt") + "</tds:NTPManual>";
                        }
                        else if (has("SetNTP"))
                        {
                            NtpConfiguration ntp;
                            ntp.from_dhcp = detail_xml::first_text(request.body, "FromDHCP") == "true";
                            for (const auto field : {"DNSname", "IPv4Address", "IPv6Address"})
                            {
                                detail_xml::for_each_element(
                                    request.body,
                                    field,
                                    [&](const detail_xml::Element& host)
                                    { ntp.servers.push_back(detail_xml::text(request.body, host)); }
                                );
                            }
                            state->ntp = std::move(ntp);
                            name = "SetNTP";
                        }
                        else if (has("SetScopes") || has("AddScopes") || has("RemoveScopes"))
                        {
                            auto& scopes = state->scopes;
                            const auto find = [&](const std::string& uri)
                            {
                                return std::find_if(scopes.begin(), scopes.end(),
                                                    [&](const auto& scope) { return scope.uri == uri; });
                            };
                            if (has("SetScopes"))
                            {
                                for (const auto& item : items)
                                {
                                    const auto existing = find(item);
                                    if (existing != scopes.end() && existing->fixed)
                                        return fault("ter:OperationProhibited", "ter:ScopeOverwrite");
                                }
                                std::erase_if(scopes, [](const auto& scope) { return !scope.fixed; });
                                for (const auto& item : items) scopes.push_back({item, false});
                                name = "SetScopes";
                            }
                            else if (has("AddScopes"))
                            {
                                for (const auto& item : items)
                                    if (find(item) == scopes.end()) scopes.push_back({item, false});
                                name = "AddScopes";
                            }
                            else
                            {
                                // Validate every item first so a refusal changes nothing.
                                for (const auto& item : items)
                                {
                                    const auto existing = find(item);
                                    if (existing == scopes.end())
                                        return fault("ter:InvalidArgVal", "ter:NoScope");
                                    if (existing->fixed)
                                        return fault("ter:OperationProhibited", "ter:FixedScope");
                                }
                                for (const auto& item : items)
                                {
                                    scopes.erase(find(item));
                                    body += tag("ScopeItem", item);
                                }
                                name = "RemoveScopes";
                            }
                        }
                        else if (request.body.find("GetDeviceInformation") != request.body.npos)
                        {
                            name = "GetDeviceInformation";
                            body = tag("Manufacturer", info.manufacturer) +
                                   tag("Model", info.model) +
                                   tag("FirmwareVersion", info.firmware_version) +
                                   tag("SerialNumber", info.serial_number) +
                                   tag("HardwareId", info.hardware_id);
                        }
                        else if (request.body.find("GetSystemDateAndTime") != request.body.npos)
                        {
                            name = "GetSystemDateAndTime";
                            body = "<tds:SystemDateAndTime>" +
                                   tag("DateTimeType",
                                       state->clock_type == DateTimeType::ntp ? "NTP" : "Manual",
                                       "tt") +
                                   tag("DaylightSavings", state->daylight_savings ? "true" : "false",
                                       "tt") +
                                   "<tt:TimeZone>" + tag("TZ", state->time_zone, "tt") +
                                   "</tt:TimeZone>" +
                                   utc_date_and_time(std::chrono::system_clock::now() +
                                                     state->clock_offset) +
                                   "</tds:SystemDateAndTime>";
                        }
                        else if (request.body.find("GetScopes") != request.body.npos)
                        {
                            name = "GetScopes";
                            for (const auto& scope : state->scopes)
                            {
                                body += "<tds:Scopes>" +
                                        tag("ScopeDef", scope.fixed ? "Fixed" : "Configurable", "tt") +
                                        tag("ScopeItem", scope.uri, "tt") + "</tds:Scopes>";
                            }
                        }
                        else if (request.body.find("GetServices") != request.body.npos)
                        {
                            name = "GetServices";
                            for (auto& s : services)
                            {
                                body += "<tds:Service>" + tag("Namespace", s.namespace_uri) +
                                        tag("XAddr", s.xaddr) + "</tds:Service>";
                            }
                        }
                        else if (request.body.find("GetCapabilities") != request.body.npos)
                        {
                            name = "GetCapabilities";
                            if (caps.media_xaddr)
                            {
                                body +=
                                    "<tt:Media>" + tag("XAddr", *caps.media_xaddr) + "</tt:Media>";
                            }
                            if (caps.ptz_xaddr)
                            {
                                body += "<tt:PTZ>" + tag("XAddr", *caps.ptz_xaddr) + "</tt:PTZ>";
                            }
                            if (caps.events_xaddr)
                            {
                                body += "<tt:Events>" + tag("XAddr", *caps.events_xaddr) +
                                        "</tt:Events>";
                            }
                            if (caps.analytics_xaddr)
                            {
                                body += "<tt:Analytics>" + tag("XAddr", *caps.analytics_xaddr) +
                                        "</tt:Analytics>";
                            }
                            if (caps.imaging_xaddr)
                            {
                                body += "<tt:Imaging>" + tag("XAddr", *caps.imaging_xaddr) +
                                        "</tt:Imaging>";
                            }
                        }
                        else
                        {
                            lock.unlock();
                            done(Error{ErrorCategory::unsupported});
                            return;
                        }
                        lock.unlock();
                        auto xml =
                            "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
                            "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
                            "xmlns:tt=\"http://www.onvif.org/ver10/schema\"><s:Body><tds:" +
                            name + "Response>" + body + "</tds:" + name +
                            "Response></s:Body></s:Envelope>";
                        done(HttpResponse{200, std::move(xml), "application/soap+xml"});
                    }
                );
            }
        );
    }
    void VirtualDeviceTransport::add_profile(
        MediaProfile p, MediaUri stream, MediaUri snapshot, PtzSpaces spaces
    )
    {
        std::lock_guard l(service_state_->mutex);
        VirtualServiceState::ProfileState s{
            p,
            std::move(stream),
            std::move(snapshot),
            spaces,
            {{0.0, 0.0, 0.0}, false, std::nullopt, std::nullopt, true},
            {}
        };
        service_state_->profiles.insert_or_assign(p.token, std::move(s));
    }
    void VirtualDeviceTransport::add_preset(ProfileToken profile, Preset preset)
    {
        std::lock_guard l(service_state_->mutex);
        auto i = service_state_->profiles.find(profile);
        if (i != service_state_->profiles.end())
        {
            i->second.presets.push_back(std::move(preset));
        }
    }
    void VirtualDeviceTransport::emit(Event event)
    {
        std::lock_guard l(service_state_->mutex);
        service_state_->events.push_back(std::move(event));
    }
    void VirtualDeviceTransport::fail_next_move(Error error)
    {
        std::lock_guard lock(service_state_->mutex);
        service_state_->next_move_error = std::move(error);
    }
    void VirtualDeviceTransport::set_stop_failure(std::optional<Error> error)
    {
        std::lock_guard lock(service_state_->mutex);
        service_state_->stop_error = std::move(error);
    }
    void VirtualDeviceTransport::set_stop_delay(std::chrono::milliseconds delay)
    {
        std::lock_guard lock(service_state_->mutex);
        service_state_->stop_delay = std::max(delay, std::chrono::milliseconds::zero());
    }
    std::size_t VirtualDeviceTransport::stop_attempts(const ProfileToken& profile) const
    {
        std::lock_guard lock(service_state_->mutex);
        const auto found = service_state_->stop_attempts.find(profile);
        return found == service_state_->stop_attempts.end() ? 0 : found->second;
    }
    Result<std::vector<MediaProfile>> VirtualDeviceTransport::profiles()
    {
        std::lock_guard l(service_state_->mutex);
        std::vector<MediaProfile> out;
        for (auto& [_, p] : service_state_->profiles)
        {
            out.push_back(p.profile);
        }
        return out;
    }
    Result<MediaUri> VirtualDeviceTransport::stream_uri(const ProfileToken& t)
    {
        std::lock_guard l(service_state_->mutex);
        auto i = service_state_->profiles.find(t);
        if (i == service_state_->profiles.end())
        {
            return Error{
                ErrorCategory::invalid_argument, OperationId::get_stream_uri, "unknown profile"
            };
        }
        return i->second.stream;
    }
    Result<MediaUri> VirtualDeviceTransport::snapshot_uri(const ProfileToken& t)
    {
        std::lock_guard l(service_state_->mutex);
        auto i = service_state_->profiles.find(t);
        if (i == service_state_->profiles.end())
        {
            return Error{
                ErrorCategory::invalid_argument, OperationId::get_snapshot_uri, "unknown profile"
            };
        }
        return i->second.snapshot;
    }
    Result<PtzSpaces> VirtualDeviceTransport::ptz_spaces(const ProfileToken& t)
    {
        std::lock_guard l(service_state_->mutex);
        auto i = service_state_->profiles.find(t);
        if (i == service_state_->profiles.end())
        {
            return Error{
                ErrorCategory::invalid_argument, OperationId::ptz_status, "unknown profile"
            };
        }
        return i->second.spaces;
    }
    Result<PtzStatus> VirtualDeviceTransport::ptz_status(const ProfileToken& t)
    {
        std::lock_guard l(service_state_->mutex);
        auto i = service_state_->profiles.find(t);
        if (i == service_state_->profiles.end())
        {
            return Error{
                ErrorCategory::invalid_argument, OperationId::ptz_status, "unknown profile"
            };
        }
        return i->second.status;
    }
    Result<MoveResult>
    VirtualDeviceTransport::move(const ProfileToken& t, PtzVector v, bool relative)
    {
        std::lock_guard l(service_state_->mutex);
        if (service_state_->next_move_error)
        {
            auto error = std::move(*service_state_->next_move_error);
            service_state_->next_move_error.reset();
            return error;
        }
        auto i = service_state_->profiles.find(t);
        if (i == service_state_->profiles.end())
        {
            return Error{
                ErrorCategory::invalid_argument,
                relative ? OperationId::relative_move : OperationId::continuous_move,
                "unknown profile"
            };
        }
        auto apply = [relative](std::optional<double>& dst, const std::optional<double>& src)
        {
            if (src)
            {
                dst = relative ? std::clamp(dst.value_or(0.0) + *src, -1.0, 1.0)
                               : std::clamp(*src, -1.0, 1.0);
            }
        };
        apply(i->second.status.position.pan, v.pan);
        apply(i->second.status.position.tilt, v.tilt);
        apply(i->second.status.position.zoom, v.zoom);
        i->second.status.moving = true;
        return MoveResult{false, {}, {}};
    }
    Result<MoveResult> VirtualDeviceTransport::absolute_move(
        const ProfileToken& t, PtzVector position, std::optional<PtzVector>
    )
    {
        std::lock_guard l(service_state_->mutex);
        if (service_state_->next_move_error)
        {
            auto error = std::move(*service_state_->next_move_error);
            service_state_->next_move_error.reset();
            return error;
        }
        auto i = service_state_->profiles.find(t);
        if (i == service_state_->profiles.end())
        {
            return Error{
                ErrorCategory::invalid_argument, OperationId::absolute_move, "unknown profile"
            };
        }
        const auto apply = [](std::optional<double>& destination, std::optional<double> value)
        {
            if (value) destination = std::clamp(*value, -1.0, 1.0);
        };
        apply(i->second.status.position.pan, position.pan);
        apply(i->second.status.position.tilt, position.tilt);
        apply(i->second.status.position.zoom, position.zoom);
        i->second.status.moving = true;
        return MoveResult{false, {}, {}};
    }
    Result<void> VirtualDeviceTransport::stop(const ProfileToken& t)
    {
        std::unique_lock l(service_state_->mutex);
        ++service_state_->stop_attempts[t];
        const auto delay = service_state_->stop_delay;
        l.unlock();
        if (delay > std::chrono::milliseconds::zero()) std::this_thread::sleep_for(delay);
        l.lock();
        if (service_state_->stop_error) return *service_state_->stop_error;
        auto i = service_state_->profiles.find(t);
        if (i == service_state_->profiles.end())
        {
            return Error{ErrorCategory::invalid_argument, OperationId::ptz_stop, "unknown profile"};
        }
        i->second.status.moving = false;
        return {};
    }
    Result<std::vector<Preset>> VirtualDeviceTransport::presets(const ProfileToken& t)
    {
        std::lock_guard l(service_state_->mutex);
        auto i = service_state_->profiles.find(t);
        if (i == service_state_->profiles.end())
        {
            return Error{
                ErrorCategory::invalid_argument, OperationId::get_presets, "unknown profile"
            };
        }
        return i->second.presets;
    }
    Result<void> VirtualDeviceTransport::goto_preset(const ProfileToken& t, const PresetToken& p)
    {
        std::lock_guard l(service_state_->mutex);
        auto i = service_state_->profiles.find(t);
        if (i == service_state_->profiles.end())
        {
            return Error{
                ErrorCategory::invalid_argument, OperationId::goto_preset, "unknown profile"
            };
        }
        auto j = std::find_if(
            i->second.presets.begin(),
            i->second.presets.end(),
            [&](auto& v)
            {
                return v.token == p;
            }
        );
        if (j == i->second.presets.end())
        {
            return Error{
                ErrorCategory::invalid_argument, OperationId::goto_preset, "unknown preset"
            };
        }
        i->second.status.position = j->position;
        i->second.status.moving = false;
        return {};
    }
    Result<std::vector<Event>> VirtualDeviceTransport::pull_events(std::size_t maximum)
    {
        std::lock_guard l(service_state_->mutex);
        std::vector<Event> out;
        while (!service_state_->events.empty() && out.size() < maximum)
        {
            out.push_back(std::move(service_state_->events.front()));
            service_state_->events.pop_front();
        }
        return out;
    }
    Result<void> VirtualDeviceTransport::renew_events()
    {
        return {};
    }
    Result<void> VirtualDeviceTransport::close_events()
    {
        return {};
    }
    namespace
    {
        /// ConcreteSet matching: an exact topic, or a `//.` expression's sub-topics.
        bool topic_matches(const std::string& topic, const std::vector<std::string>& expressions)
        {
            if (expressions.empty()) return true;
            for (const auto& expression : expressions)
            {
                if (topic == expression) return true;
                constexpr std::string_view subtree = "//.";
                if (expression.ends_with(subtree))
                {
                    const auto root = expression.substr(0, expression.size() - subtree.size());
                    if (topic == root || topic.starts_with(root + "/")) return true;
                }
            }
            return false;
        }

        Result<void> replace_parameters(
            std::vector<AnalyticsConfig>& stored, const std::vector<AnalyticsConfig>& changes,
            OperationId operation
        )
        {
            for (const auto& change : changes)
            {
                if (std::none_of(stored.begin(), stored.end(),
                                 [&](const auto& config) { return config.name == change.name; }))
                    return Error{ErrorCategory::invalid_argument, operation,
                                 "no analytics entry named " + change.name};
            }
            for (const auto& change : changes)
                for (auto& config : stored)
                    if (config.name == change.name) config = change;
            return {};
        }
    } // namespace
    namespace
    {
        Error unknown(OperationId operation, std::string what)
        {
            return Error{ErrorCategory::invalid_argument, operation, "unknown " + std::move(what)};
        }
    } // namespace
    Result<std::string> VirtualDeviceTransport::call(
        OnvifService service, const std::string& body, const std::string& response_name
    )
    {
        if (service != OnvifService::device)
            return Error{ErrorCategory::unsupported, OperationId::custom,
                         "the virtual device answers custom requests on the Device service only"};
        HttpRequest request{
            "http://virtual/onvif/device_service", soap_envelope(OperationId::custom, body), {},
            std::chrono::steady_clock::now() + std::chrono::seconds(5)
        };
        std::promise<Result<HttpResponse>> answered;
        auto future = answered.get_future();
        send(std::move(request), CancellationSource{}.token())
            .start([&answered](Result<HttpResponse> result) { answered.set_value(std::move(result)); });
        auto response = future.get();
        if (!response) return response.error();
        return detail::soap_response_content(response.value(), response_name, OperationId::custom);
    }
    std::size_t VirtualDeviceTransport::reboot_count() const
    {
        std::lock_guard lock(service_state_->mutex);
        return service_state_->reboots;
    }
    void VirtualDeviceTransport::set_ptz_configuration(ProfileToken profile, PtzConfiguration configuration)
    {
        std::lock_guard lock(service_state_->mutex);
        service_state_->ptz_configurations[std::move(profile)] = std::move(configuration);
    }
    Result<PresetToken> VirtualDeviceTransport::set_preset(
        const ProfileToken& profile, const std::string& name, const PresetToken& token
    )
    {
        std::lock_guard lock(service_state_->mutex);
        const auto found = service_state_->profiles.find(profile);
        if (found == service_state_->profiles.end()) return unknown(OperationId::set_preset, "profile");
        auto& presets = found->second.presets;
        const auto position = found->second.status.position;
        if (!token.empty())
        {
            const auto existing = std::find_if(presets.begin(), presets.end(),
                                               [&](const auto& preset) { return preset.token == token; });
            if (existing == presets.end()) return unknown(OperationId::set_preset, "preset");
            existing->position = position;
            if (!name.empty()) existing->name = name;
            return token;
        }
        auto created = "preset-" + std::to_string(service_state_->next_token++);
        presets.push_back({created, name, position});
        return created;
    }
    Result<void> VirtualDeviceTransport::remove_preset(const ProfileToken& profile, const PresetToken& token)
    {
        std::lock_guard lock(service_state_->mutex);
        const auto found = service_state_->profiles.find(profile);
        if (found == service_state_->profiles.end()) return unknown(OperationId::remove_preset, "profile");
        if (std::erase_if(found->second.presets, [&](const auto& preset) { return preset.token == token; }) == 0)
            return unknown(OperationId::remove_preset, "preset");
        return {};
    }
    Result<std::vector<PresetTour>> VirtualDeviceTransport::preset_tours(const ProfileToken& profile)
    {
        std::lock_guard lock(service_state_->mutex);
        if (!service_state_->profiles.contains(profile)) return unknown(OperationId::get_preset_tours, "profile");
        return service_state_->tours[profile];
    }
    Result<std::string> VirtualDeviceTransport::create_preset_tour(const ProfileToken& profile)
    {
        std::lock_guard lock(service_state_->mutex);
        if (!service_state_->profiles.contains(profile))
            return unknown(OperationId::create_preset_tour, "profile");
        auto token = "tour-" + std::to_string(service_state_->next_token++);
        PresetTour tour;
        tour.token = token;
        tour.state = PresetTourState::idle;
        service_state_->tours[profile].push_back(std::move(tour));
        return token;
    }
    Result<void> VirtualDeviceTransport::modify_preset_tour(const ProfileToken& profile, const PresetTour& tour)
    {
        std::lock_guard lock(service_state_->mutex);
        auto& tours = service_state_->tours[profile];
        const auto existing = std::find_if(tours.begin(), tours.end(),
                                           [&](const auto& item) { return item.token == tour.token; });
        if (existing == tours.end()) return unknown(OperationId::modify_preset_tour, "tour");
        const auto& presets = service_state_->profiles[profile].presets;
        for (const auto& spot : tour.spots)
            if (std::none_of(presets.begin(), presets.end(),
                             [&](const auto& preset) { return preset.token == spot.preset; }))
                return unknown(OperationId::modify_preset_tour, "preset " + spot.preset);
        existing->name = tour.name;
        existing->auto_start = tour.auto_start;
        existing->spots = tour.spots;
        return {};
    }
    Result<void> VirtualDeviceTransport::operate_preset_tour(
        const ProfileToken& profile, const std::string& token, PresetTourOperation operation
    )
    {
        std::lock_guard lock(service_state_->mutex);
        auto& tours = service_state_->tours[profile];
        const auto existing = std::find_if(tours.begin(), tours.end(),
                                           [&](const auto& item) { return item.token == token; });
        if (existing == tours.end()) return unknown(OperationId::operate_preset_tour, "tour");
        existing->state = operation == PresetTourOperation::start  ? PresetTourState::touring
                          : operation == PresetTourOperation::stop ? PresetTourState::idle
                                                                   : PresetTourState::paused;
        return {};
    }
    Result<void> VirtualDeviceTransport::remove_preset_tour(const ProfileToken& profile, const std::string& token)
    {
        std::lock_guard lock(service_state_->mutex);
        if (std::erase_if(service_state_->tours[profile], [&](const auto& item) { return item.token == token; }) == 0)
            return unknown(OperationId::remove_preset_tour, "tour");
        return {};
    }
    Result<PtzConfiguration> VirtualDeviceTransport::ptz_configuration(const ProfileToken& profile)
    {
        std::lock_guard lock(service_state_->mutex);
        const auto found = service_state_->ptz_configurations.find(profile);
        if (found == service_state_->ptz_configurations.end())
            return unknown(OperationId::get_ptz_configuration, "PTZ configuration");
        return found->second;
    }
    Result<void> VirtualDeviceTransport::set_ptz_direction(
        const ProfileToken& profile, const PtzDirection& direction, bool
    )
    {
        constexpr auto operation = OperationId::set_ptz_configuration;
        std::lock_guard lock(service_state_->mutex);
        const auto found = service_state_->ptz_configurations.find(profile);
        if (found == service_state_->ptz_configurations.end())
            return unknown(operation, "PTZ configuration");
        auto& configuration = found->second;
        const auto accepted = [](const std::optional<std::string>& mode, const std::vector<std::string>& modes)
        { return !mode || std::find(modes.begin(), modes.end(), *mode) != modes.end(); };
        if (!accepted(direction.eflip, configuration.eflip_modes) ||
            !accepted(direction.reverse, configuration.reverse_modes))
            return Error{ErrorCategory::invalid_argument, operation, "direction mode rejected"};
        if (direction.eflip) configuration.direction.eflip = direction.eflip;
        if (direction.reverse) configuration.direction.reverse = direction.reverse;
        return {};
    }
    void VirtualDeviceTransport::set_profile_configuration(ProfileConfiguration profile)
    {
        std::lock_guard lock(service_state_->mutex);
        auto& profiles = service_state_->profile_configurations;
        std::erase_if(profiles, [&](const auto& entry) { return entry.token == profile.token; });
        profiles.push_back(std::move(profile));
    }
    void VirtualDeviceTransport::set_video_encoder_options(
        std::string configuration, VideoEncoderOptions options
    )
    {
        std::lock_guard lock(service_state_->mutex);
        service_state_->encoder_options[std::move(configuration)] = std::move(options);
    }
    Result<std::vector<ProfileConfiguration>> VirtualDeviceTransport::profile_configurations()
    {
        std::lock_guard lock(service_state_->mutex);
        return service_state_->profile_configurations;
    }
    Result<VideoEncoderOptions>
    VirtualDeviceTransport::video_encoder_options(const std::string& configuration)
    {
        std::lock_guard lock(service_state_->mutex);
        const auto found = service_state_->encoder_options.find(configuration);
        if (found == service_state_->encoder_options.end())
            return Error{ErrorCategory::invalid_argument, OperationId::get_video_encoder_options,
                         "no encoder options for " + configuration};
        return found->second;
    }
    Result<void> VirtualDeviceTransport::set_video_encoder_configuration(
        const VideoEncoderConfiguration& configuration, bool
    )
    {
        constexpr auto operation = OperationId::set_video_encoder_configuration;
        std::lock_guard lock(service_state_->mutex);
        const auto options = service_state_->encoder_options.find(configuration.token);
        if (options != service_state_->encoder_options.end())
        {
            const auto& encodings = options->second.encodings;
            const auto encoding = std::find_if(encodings.begin(), encodings.end(), [&](const auto& item)
                                               { return item.encoding == configuration.encoding; });
            const auto within = [](double value, const IntegerRange& range)
            { return value >= range.minimum && value <= range.maximum; };
            if (encoding == encodings.end() ||
                std::find(encoding->resolutions.begin(), encoding->resolutions.end(),
                          configuration.resolution) == encoding->resolutions.end() ||
                !within(configuration.quality, options->second.quality) ||
                !within(configuration.frame_rate_limit, encoding->frame_rate) ||
                (encoding->bitrate && !within(configuration.bitrate_limit, *encoding->bitrate)))
                return Error{ErrorCategory::invalid_argument, operation, "encoder setting rejected"};
        }
        bool found = false;
        for (auto& profile : service_state_->profile_configurations)
        {
            if (profile.video_encoder && profile.video_encoder->token == configuration.token)
            {
                profile.video_encoder = configuration;
                found = true;
            }
        }
        if (!found)
            return Error{ErrorCategory::invalid_argument, operation,
                         "no encoder configuration " + configuration.token};
        return {};
    }
    void VirtualDeviceTransport::set_video_source(
        VideoSource source, ImagingSettings settings, ImagingOptions options
    )
    {
        std::lock_guard lock(service_state_->mutex);
        auto& imaging = service_state_->imaging;
        std::erase_if(imaging, [&](const auto& entry) { return entry.source.token == source.token; });
        imaging.push_back({std::move(source), std::move(settings), std::move(options)});
    }
    Result<std::vector<VideoSource>> VirtualDeviceTransport::video_sources()
    {
        std::lock_guard lock(service_state_->mutex);
        std::vector<VideoSource> output;
        for (const auto& entry : service_state_->imaging) output.push_back(entry.source);
        return output;
    }
    Result<ImagingSettings> VirtualDeviceTransport::imaging_settings(const std::string& source)
    {
        std::lock_guard lock(service_state_->mutex);
        for (const auto& entry : service_state_->imaging)
            if (entry.source.token == source) return entry.settings;
        return Error{ErrorCategory::invalid_argument, OperationId::get_imaging_settings,
                     "no video source " + source};
    }
    Result<ImagingOptions> VirtualDeviceTransport::imaging_options(const std::string& source)
    {
        std::lock_guard lock(service_state_->mutex);
        for (const auto& entry : service_state_->imaging)
            if (entry.source.token == source) return entry.options;
        return Error{ErrorCategory::invalid_argument, OperationId::get_imaging_options,
                     "no video source " + source};
    }
    Result<void> VirtualDeviceTransport::set_imaging_settings(
        const std::string& source, const ImagingSettings& settings, bool
    )
    {
        constexpr auto operation = OperationId::set_imaging_settings;
        std::lock_guard lock(service_state_->mutex);
        for (auto& entry : service_state_->imaging)
        {
            if (entry.source.token != source) continue;
            const auto& options = entry.options;
            const auto in_range = [](const std::optional<double>& value,
                                     const std::optional<ImagingRange>& range)
            {
                return !value || (range && *value >= range->minimum && *value <= range->maximum);
            };
            const auto allowed = [](const std::optional<std::string>& value,
                                    const std::vector<std::string>& modes)
            {
                return !value || std::find(modes.begin(), modes.end(), *value) != modes.end();
            };
            // A real device faults the whole request, so validate before changing any field.
            if (!in_range(settings.brightness, options.brightness) ||
                !in_range(settings.color_saturation, options.color_saturation) ||
                !in_range(settings.contrast, options.contrast) ||
                !in_range(settings.sharpness, options.sharpness) ||
                !allowed(settings.ir_cut_filter, options.ir_cut_filter_modes) ||
                !allowed(settings.backlight_compensation, options.backlight_compensation_modes) ||
                !allowed(settings.wide_dynamic_range, options.wide_dynamic_range_modes) ||
                !allowed(settings.white_balance, options.white_balance_modes))
                return Error{ErrorCategory::invalid_argument, operation, "imaging setting rejected"};
            auto& stored = entry.settings;
            const auto merge = [](auto& target, const auto& value) { if (value) target = value; };
            merge(stored.brightness, settings.brightness);
            merge(stored.color_saturation, settings.color_saturation);
            merge(stored.contrast, settings.contrast);
            merge(stored.sharpness, settings.sharpness);
            merge(stored.ir_cut_filter, settings.ir_cut_filter);
            merge(stored.backlight_compensation, settings.backlight_compensation);
            merge(stored.wide_dynamic_range, settings.wide_dynamic_range);
            merge(stored.white_balance, settings.white_balance);
            return {};
        }
        return Error{ErrorCategory::invalid_argument, operation, "no video source " + source};
    }
    void VirtualDeviceTransport::set_event_topics(std::vector<EventTopic> topics)
    {
        std::lock_guard lock(service_state_->mutex);
        service_state_->topics = std::move(topics);
    }
    void VirtualDeviceTransport::set_analytics(
        std::string configuration,
        std::vector<AnalyticsConfig> modules,
        std::vector<AnalyticsConfig> rules
    )
    {
        std::lock_guard lock(service_state_->mutex);
        service_state_->analytics[std::move(configuration)] = {std::move(modules), std::move(rules)};
    }
    Result<PullPointSubscriptionInfo> VirtualDeviceTransport::create_filtered_event_subscription(
        std::chrono::seconds requested_lifetime, const std::vector<std::string>& topics
    )
    {
        std::lock_guard lock(service_state_->mutex);
        auto token = "virtual-" + std::to_string(service_state_->next_subscription++);
        service_state_->subscription_topics[token] = topics;
        return PullPointSubscriptionInfo{
            std::move(token), {}, std::chrono::system_clock::now() + requested_lifetime
        };
    }
    Result<std::vector<Event>> VirtualDeviceTransport::pull_event_subscription(
        const std::string& token, std::chrono::milliseconds, std::size_t maximum
    )
    {
        std::lock_guard lock(service_state_->mutex);
        const auto filter = service_state_->subscription_topics.find(token);
        std::vector<Event> output;
        auto& events = service_state_->events;
        while (!events.empty() && output.size() < maximum)
        {
            auto event = std::move(events.front());
            events.pop_front();
            if (filter == service_state_->subscription_topics.end() ||
                topic_matches(event.topic, filter->second))
                output.push_back(std::move(event));
        }
        return output;
    }
    Result<std::vector<EventTopic>> VirtualDeviceTransport::event_topics()
    {
        std::lock_guard lock(service_state_->mutex);
        return service_state_->topics;
    }
    Result<std::vector<AnalyticsConfig>>
    VirtualDeviceTransport::analytics_modules(const std::string& configuration)
    {
        std::lock_guard lock(service_state_->mutex);
        const auto found = service_state_->analytics.find(configuration);
        if (found == service_state_->analytics.end())
            return Error{ErrorCategory::invalid_argument, OperationId::get_analytics_modules,
                         "no analytics configuration " + configuration};
        return found->second.modules;
    }
    Result<void> VirtualDeviceTransport::modify_analytics_modules(
        const std::string& configuration, const std::vector<AnalyticsConfig>& modules
    )
    {
        std::lock_guard lock(service_state_->mutex);
        const auto found = service_state_->analytics.find(configuration);
        if (found == service_state_->analytics.end())
            return Error{ErrorCategory::invalid_argument, OperationId::modify_analytics_modules,
                         "no analytics configuration " + configuration};
        return replace_parameters(found->second.modules, modules, OperationId::modify_analytics_modules);
    }
    Result<std::vector<AnalyticsConfig>>
    VirtualDeviceTransport::analytics_rules(const std::string& configuration)
    {
        std::lock_guard lock(service_state_->mutex);
        const auto found = service_state_->analytics.find(configuration);
        if (found == service_state_->analytics.end())
            return Error{ErrorCategory::invalid_argument, OperationId::get_analytics_rules,
                         "no analytics configuration " + configuration};
        return found->second.rules;
    }
    Result<void> VirtualDeviceTransport::modify_analytics_rules(
        const std::string& configuration, const std::vector<AnalyticsConfig>& rules
    )
    {
        std::lock_guard lock(service_state_->mutex);
        const auto found = service_state_->analytics.find(configuration);
        if (found == service_state_->analytics.end())
            return Error{ErrorCategory::invalid_argument, OperationId::modify_analytics_rules,
                         "no analytics configuration " + configuration};
        return replace_parameters(found->second.rules, rules, OperationId::modify_analytics_rules);
    }
} // namespace wlonvif

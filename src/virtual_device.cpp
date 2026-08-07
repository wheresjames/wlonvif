#include "dispatch.hpp"
#include "xml_scan.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <deque>
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
        /// Serialize the current UTC time the way a Device service reports it.
        std::string utc_date_and_time()
        {
            const auto now = std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now()
            );
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
    }
    Task<Result<HttpResponse>>
    VirtualDeviceTransport::send(HttpRequest request, CancellationToken token)
    {
        auto rt = runtime_;
        auto info = info_;
        auto scopes = scopes_;
        auto services = services_;
        auto caps = capabilities_;
        return Task<Result<HttpResponse>>(
            [rt,
             request = std::move(request),
             token,
             info = std::move(info),
             scopes = std::move(scopes),
             services = std::move(services),
             caps = std::move(caps)](auto done) mutable
            {
                detail::dispatch(
                    rt,
                    [request = std::move(request),
                     token,
                     info = std::move(info),
                     scopes = std::move(scopes),
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
                        if (request.body.find("GetDeviceInformation") != request.body.npos)
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
                            body = "<tds:SystemDateAndTime>" + utc_date_and_time() +
                                   "</tds:SystemDateAndTime>";
                        }
                        else if (request.body.find("GetScopes") != request.body.npos)
                        {
                            name = "GetScopes";
                            for (auto& s : scopes)
                            {
                                body += "<tds:Scopes><tt:ScopeItem>" + esc(s) +
                                        "</tt:ScopeItem></tds:Scopes>";
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
                        }
                        else
                        {
                            done(Error{ErrorCategory::unsupported});
                            return;
                        }
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
        return MoveResult{true, {}};
    }
    Result<MoveResult> VirtualDeviceTransport::absolute_move(
        const ProfileToken& t, PtzVector position, std::optional<PtzVector>
    )
    {
        std::lock_guard l(service_state_->mutex);
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
        return MoveResult{true, {}};
    }
    Result<void> VirtualDeviceTransport::stop(const ProfileToken& t)
    {
        std::lock_guard l(service_state_->mutex);
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
} // namespace wlonvif

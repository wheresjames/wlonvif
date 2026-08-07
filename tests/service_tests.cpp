#include <cmath>
#include <future>
#include <iostream>
#include <mutex>
#include <wlonvif/wlonvif.hpp>
using namespace wlonvif;
namespace
{
    class NetworkFixtureTransport final : public HttpTransport
    {
      public:
        Task<Result<HttpResponse>> send(HttpRequest request, CancellationToken) override
        {
            return Task<Result<HttpResponse>>(
                [this, request = std::move(request)](auto completion) mutable
                {
                    if (request.method == HttpMethod::get)
                    {
                        completion(HttpResponse{200, "JPEG", "image/jpeg"});
                        return;
                    }
                    std::string response;
                    if (request.body.find("GetCapabilities") != request.body.npos)
                        response = "<tds:GetCapabilitiesResponse><tds:Capabilities>"
                                   "<tt:Media><tt:XAddr>http://camera:8000/media</tt:XAddr></tt:Media>"
                                   "<tt:PTZ><tt:XAddr>http://camera:8000/ptz</tt:XAddr></tt:PTZ>"
                                   "<tt:Events><tt:XAddr>http://camera:8000/events</tt:XAddr></tt:Events>"
                                   "</tds:Capabilities></tds:GetCapabilitiesResponse>";
                    else if (request.body.find("GetProfiles") != request.body.npos)
                        response = "<trt:GetProfilesResponse><trt:Profiles token='main'>"
                                   "<tt:Name>Main</tt:Name><tt:PTZConfiguration token='ptz-main'/>"
                                   "</trt:Profiles></trt:GetProfilesResponse>";
                    else if (request.body.find("GetStreamUri") != request.body.npos)
                        response = "<trt:GetStreamUriResponse><trt:MediaUri>"
                                   "<tt:Uri>rtsp://camera/main</tt:Uri>"
                                   "</trt:MediaUri></trt:GetStreamUriResponse>";
                    else if (request.body.find("GetSnapshotUri") != request.body.npos)
                        response = "<trt:GetSnapshotUriResponse><trt:MediaUri>"
                                   "<tt:Uri>http://camera:8000/snapshot.jpg</tt:Uri>"
                                   "</trt:MediaUri></trt:GetSnapshotUriResponse>";
                    else if (request.body.find("GetStatus") != request.body.npos)
                        response = "<tptz:GetStatusResponse><tptz:PTZStatus>"
                                   "<tt:Position><tt:PanTilt x='0.2' y='-0.1'/><tt:Zoom x='0.3'/>"
                                   "</tt:Position><tt:MoveStatus>MOVING</tt:MoveStatus>"
                                   "</tptz:PTZStatus></tptz:GetStatusResponse>";
                    else if (request.body.find("GetConfigurationOptions") != request.body.npos)
                    {
                        ++configuration_requests;
                        response = "<tptz:GetConfigurationOptionsResponse><tptz:PTZConfigurationOptions>"
                                   "<tt:Spaces><tt:ContinuousPanTiltVelocitySpace>"
                                   "<tt:URI>urn:velocity</tt:URI>"
                                   "<tt:XRange><tt:Min>-2</tt:Min><tt:Max>2</tt:Max></tt:XRange>"
                                   "<tt:YRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:YRange>"
                                   "</tt:ContinuousPanTiltVelocitySpace>"
                                   "<tt:ContinuousZoomVelocitySpace><tt:XRange>"
                                   "<tt:Min>0</tt:Min><tt:Max>1</tt:Max></tt:XRange>"
                                   "</tt:ContinuousZoomVelocitySpace>"
                                   "<tt:AbsolutePanTiltPositionSpace><tt:URI>urn:absolute</tt:URI>"
                                   "<tt:XRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:XRange>"
                                   "<tt:YRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:YRange>"
                                   "</tt:AbsolutePanTiltPositionSpace></tt:Spaces>"
                                   "</tptz:PTZConfigurationOptions></tptz:GetConfigurationOptionsResponse>";
                    }
                    else if (request.body.find("GetPresets") != request.body.npos)
                        response = "<tptz:GetPresetsResponse><tptz:Preset token='home'>"
                                   "<tt:Name>Home</tt:Name><tptz:PTZPosition>"
                                   "<tt:PanTilt x='0' y='0'/></tptz:PTZPosition>"
                                   "</tptz:Preset></tptz:GetPresetsResponse>";
                    else if (request.body.find("ContinuousMove") != request.body.npos)
                        response = "<tptz:ContinuousMoveResponse/>";
                    else if (request.body.find("RelativeMove") != request.body.npos)
                        response = "<tptz:RelativeMoveResponse/>";
                    else if (request.body.find("AbsoluteMove") != request.body.npos)
                        response = "<tptz:AbsoluteMoveResponse/>";
                    else if (request.body.find("<tptz:Stop") != request.body.npos)
                    {
                        ++stop_requests;
                        response = "<tptz:StopResponse/>";
                    }
                    else if (request.body.find("GotoPreset") != request.body.npos)
                        response = "<tptz:GotoPresetResponse/>";
                    else if (request.body.find("CreatePullPointSubscription") != request.body.npos)
                        response = "<tev:CreatePullPointSubscriptionResponse>"
                                   "<tev:SubscriptionReference><wsa:Address>"
                                   "http://camera:8000/subscription</wsa:Address>"
                                   "</tev:SubscriptionReference>"
                                   "</tev:CreatePullPointSubscriptionResponse>";
                    else if (request.body.find("PullMessages") != request.body.npos)
                        response = "<tev:PullMessagesResponse><wsnt:NotificationMessage>"
                                   "<wsnt:Topic>tns1:Motion</wsnt:Topic><wsnt:Message>"
                                   "<tt:SimpleItem Name='State' Value='true'/></wsnt:Message>"
                                   "</wsnt:NotificationMessage></tev:PullMessagesResponse>";
                    else if (request.body.find("Renew") != request.body.npos)
                        response = "<wsnt:RenewResponse/>";
                    else if (request.body.find("Unsubscribe") != request.body.npos)
                    {
                        ++unsubscribe_requests;
                        response = "<wsnt:UnsubscribeResponse/>";
                    }
                    completion(HttpResponse{
                        200,
                        "<s:Envelope xmlns:s='http://www.w3.org/2003/05/soap-envelope' "
                        "xmlns:tds='urn:device' xmlns:tt='urn:schema' xmlns:trt='urn:media' "
                        "xmlns:tptz='urn:ptz' xmlns:tev='urn:events' xmlns:wsnt='urn:wsnt' "
                        "xmlns:wsa='urn:wsa'>"
                        "<s:Body>" + response + "</s:Body></s:Envelope>",
                        "application/soap+xml"
                    });
                }
            );
        }
        std::atomic_int stop_requests{};
        std::atomic_int configuration_requests{};
        std::atomic_int unsubscribe_requests{};
    };
}
template <class T> T wait2(Task<T> task)
{
    std::promise<T> p;
    auto f = p.get_future();
    std::move(task).start(
        [&](T v)
        {
            p.set_value(std::move(v));
        }
    );
    return f.get();
}
#define VERIFY(x)                                                                                  \
    do                                                                                             \
    {                                                                                              \
        if (!(x))                                                                                  \
        {                                                                                          \
            std::cerr << "verify failed: " #x " line " << __LINE__ << '\n';                        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
int main()
{
    auto rr = Runtime::create();
    VERIFY(rr);
    auto runtime = rr.value();
    Capabilities caps;
    caps.media_xaddr = "http://camera/media";
    caps.ptz_xaddr = "http://camera/ptz";
    caps.events_xaddr = "http://camera/events";
    auto device = std::make_shared<VirtualDeviceTransport>(
        runtime, DeviceInformation{}, std::vector<std::string>{}, std::vector<Service>{}, caps
    );
    device->add_profile(
        {"main", "Main"},
        {"rtsp://camera/main", std::chrono::seconds(60), false},
        {"http://camera/snapshot", std::chrono::seconds(30), false},
        {{-2, 2}, {-1, 3}, {0, 1}}
    );
    device->add_preset("main", {"home", "Home", {0.25, -0.5, 0.0}});
    CallOptions o{
        std::chrono::steady_clock::now() + std::chrono::seconds(5), CancellationSource{}.token()
    };
    auto connected = wait2(
        DeviceSession::connect(
            "http://camera/device", device, make_same_origin_policy(), runtime, std::nullopt, o
        )
    );
    VERIFY(connected);
    auto session = connected.value();
    auto media = session->media();
    VERIFY(media);
    auto profiles = wait2(media.value()->get_profiles(o));
    VERIFY(profiles && profiles.value().size() == 1);
    auto uri = wait2(media.value()->get_stream_uri("main", o));
    VERIFY(uri && uri.value().uri == "rtsp://camera/main");
    auto missing = wait2(media.value()->get_snapshot_uri("missing", o));
    VERIFY(!missing && missing.error().category == ErrorCategory::invalid_argument);
    auto ptz = session->ptz();
    VERIFY(ptz);
    auto normalized = normalize_ptz(0, {-2, 2});
    VERIFY(normalized && std::abs(normalized.value()) < 1e-12);
    auto native = denormalize_ptz(0.5, {-2, 2});
    VERIFY(native && std::abs(native.value() - 1) < 1e-12);
    VERIFY(!normalize_ptz(0, {1, 1}));
    auto moved = wait2(ptz.value()->continuous_move("main", {0.5, -0.25, {}}, o));
    VERIFY(moved && !moved.value().request_may_have_been_applied);
    auto status = wait2(ptz.value()->status("main", o));
    VERIFY(status && status.value().moving && status.value().position.pan == 0.5);
    VERIFY(wait2(ptz.value()->stop("main", o)));
    VERIFY(wait2(ptz.value()->continuous_move("main", {0.1, {}, {}}, o)));
    CancellationSource movement_cancelled;
    movement_cancelled.request_stop();
    auto cancelled_move = wait2(ptz.value()->relative_move(
        "main", {0.1, {}, {}}, {o.deadline, movement_cancelled.token()}
    ));
    VERIFY(!cancelled_move && cancelled_move.error().category == ErrorCategory::cancelled);
    status = wait2(ptz.value()->status("main", o));
    VERIFY(status && !status.value().moving);
    auto presets = wait2(ptz.value()->get_presets("main", o));
    VERIFY(presets && presets.value().size() == 1);
    VERIFY(wait2(ptz.value()->goto_preset("main", "home", o)));
    status = wait2(ptz.value()->status("main", o));
    VERIFY(status && status.value().position.pan == 0.25);
    DiscoverySession discovery(runtime, 1);
    DiscoveryObservation observation{
        DiscoveryKind::probe_match,
        "urn:uuid:1",
        "192.0.2.1",
        "eth0",
        {},
        {"http://camera/device"},
        1
    };
    VERIFY(wait2(discovery.inject(observation, o)));
    VERIFY(wait2(discovery.inject(observation, o)));
    auto seen = wait2(discovery.next(o));
    VERIFY(seen && seen.value().endpoint_reference == "urn:uuid:1");
    VERIFY(wait2(discovery.close(o)));
    VERIFY(!wait2(discovery.next(o)));
    auto stream = session->events(2);
    VERIFY(stream);
    device->emit({"tns1:Motion", std::chrono::system_clock::now(), "cam", "true", 1});
    device->emit({"tns1:Motion", std::chrono::system_clock::now(), "cam", "false", 2});
    device->emit({"tns1:Tamper", std::chrono::system_clock::now(), "cam", "true", 3});
    VERIFY(wait2(stream.value()->poll(3, o)));
    auto event = wait2(stream.value()->next(o));
    VERIFY(event);
    VERIFY(wait2(stream.value()->mark_reconnected(o)));
    auto health = wait2(stream.value()->next(o));
    VERIFY(health);
    VERIFY(wait2(stream.value()->close(o)));

    // Explicit PullPoint lifecycle exposes independent handle state and timing.
    auto event_client = session->event_client();
    VERIFY(event_client);
    auto subscription = wait2(event_client.value()->create_pullpoint({}, o));
    VERIFY(subscription);
    VERIFY(subscription.value()->state() == PullPointSubscriptionState::open);
    VERIFY(!subscription.value()->info().token.empty());
    device->emit({"tns1:Explicit", std::chrono::system_clock::now(), "cam", "true", 4});
    auto explicit_events = wait2(subscription.value()->pull(
        std::chrono::milliseconds(100), 4, o
    ));
    VERIFY(explicit_events && explicit_events.value().size() == 1);
    auto renewed = wait2(subscription.value()->renew(std::chrono::seconds(30), o));
    VERIFY(renewed);
    VERIFY(wait2(subscription.value()->unsubscribe(o)));
    VERIFY(subscription.value()->state() == PullPointSubscriptionState::closed);
    VERIFY(!wait2(subscription.value()->pull(std::chrono::milliseconds(100), 1, o)));

    // Automatic streams pull in the background and next() waits for data.
    ManagedEventOptions managed_options;
    managed_options.pull_timeout = std::chrono::milliseconds(50);
    managed_options.minimum_backoff = std::chrono::milliseconds(10);
    managed_options.maximum_backoff = std::chrono::milliseconds(40);
    auto automatic = wait2(event_client.value()->open_managed_stream(managed_options, o));
    VERIFY(automatic);
    device->emit({"tns1:Automatic", std::chrono::system_clock::now(), "cam", "true", 5});
    auto automatic_event = wait2(automatic.value()->next(o));
    VERIFY(automatic_event && std::holds_alternative<Event>(automatic_event.value()));
    VERIFY(wait2(automatic.value()->close(o)));
    VERIFY(library_features().pullpoint_subscription);

    auto network_transport = std::make_shared<NetworkFixtureTransport>();
    auto network_session = wait2(DeviceSession::connect(
        "http://camera/device",
        network_transport,
        make_same_host_policy(),
        runtime,
        std::nullopt,
        o
    ));
    VERIFY(network_session);
    auto network_media = network_session.value()->media();
    VERIFY(network_media);
    auto network_profiles = wait2(network_media.value()->get_profiles(o));
    VERIFY(network_profiles && network_profiles.value().size() == 1);
    VERIFY(network_profiles.value()[0].token == "main");
    auto network_uri = wait2(network_media.value()->get_stream_uri("main", o));
    VERIFY(network_uri && network_uri.value().uri == "rtsp://camera/main");
    auto snapshot_uri = wait2(network_media.value()->get_snapshot_uri("main", o));
    VERIFY(snapshot_uri && snapshot_uri.value().uri.ends_with("snapshot.jpg"));
    auto snapshot = wait2(download_binary(
        snapshot_uri.value().uri,
        "http://camera/device",
        network_transport,
        make_same_host_policy(),
        std::nullopt,
        16,
        o
    ));
    VERIFY(snapshot && snapshot.value().bytes == "JPEG");
    VERIFY(!wait2(download_binary(
        snapshot_uri.value().uri,
        "http://camera/device",
        network_transport,
        make_same_host_policy(),
        std::nullopt,
        2,
        o
    )));

    auto network_ptz = network_session.value()->ptz();
    VERIFY(network_ptz);
    auto network_status = wait2(network_ptz.value()->status("main", o));
    VERIFY(network_status && network_status.value().moving);
    VERIFY(std::abs(*network_status.value().position.pan - 0.2) < 1e-12);
    auto network_spaces = wait2(network_ptz.value()->spaces("main", o));
    VERIFY(network_spaces && network_spaces.value().pan.minimum == -2);
    VERIFY(network_spaces.value().continuous_pan_tilt.size() == 1);
    VERIFY(network_spaces.value().continuous_pan_tilt.front().uri == "urn:velocity");
    VERIFY(network_spaces.value().absolute_pan_tilt.front().uri == "urn:absolute");
    VERIFY(network_transport->configuration_requests == 1);
    auto network_presets = wait2(network_ptz.value()->get_presets("main", o));
    VERIFY(network_presets && network_presets.value().size() == 1);
    VERIFY(wait2(network_ptz.value()->relative_move("main", {0.1, {}, {}}, o)));
    VERIFY(wait2(network_ptz.value()->absolute_move(
        "main", {0.25, -0.25, {}}, std::optional<PtzVector>{{0.1, {}, {}}}, o
    )));
    VERIFY(network_transport->configuration_requests == 1);
    auto invalid_move = wait2(network_ptz.value()->continuous_move("main", {2.0, {}, {}}, o));
    VERIFY(!invalid_move && invalid_move.error().category == ErrorCategory::invalid_argument);
    VERIFY(wait2(network_ptz.value()->stop("main", o)));
    VERIFY(network_transport->stop_requests == 1);

    auto network_events = network_session.value()->events(4);
    VERIFY(network_events);
    VERIFY(wait2(network_events.value()->poll(4, o)));
    auto network_event = wait2(network_events.value()->next(o));
    VERIFY(network_event && std::holds_alternative<Event>(network_event.value()));
    VERIFY(std::get<Event>(network_event.value()).topic == "tns1:Motion");
    VERIFY(wait2(network_events.value()->poll(4, o)));
    VERIFY(wait2(network_events.value()->close(o)));
    VERIFY(network_transport->unsubscribe_requests == 1);
    VERIFY(runtime->shutdown());
    std::cout << "media, PTZ, discovery, and event tests passed\n";
}

/**
 * @file regression_tests.cpp
 * @brief Regression coverage for previously shipped defects.
 *
 * The fixtures here are deliberately hostile. The original suite passed against
 * every one of these defects because its fixtures mirrored the implementation:
 * responses used exactly the `tds:`/`tt:` prefixes the decoder matched literally,
 * so a decoder that understood nothing else still looked correct. Real devices
 * answer with `SOAP-ENV:`, `ns2:`, or no prefix at all.
 *
 * Every fixture below therefore varies the namespace prefix, plants decoy
 * elements outside the response, and uses PTZ ranges that are not already
 * normalized, so a decoder cannot pass by coincidence.
 */

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <locale>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <wlonvif/wlonvif.hpp>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#endif

using namespace wlonvif;

namespace
{
    int failures = 0;

    template <class T> T sync(Task<T> task)
    {
        struct State
        {
            std::mutex mutex;
            std::condition_variable ready;
            std::optional<T> value;
        };
        auto state = std::make_shared<State>();
        std::move(task).start(
            [state](T value)
            {
                std::lock_guard lock(state->mutex);
                state->value.emplace(std::move(value));
                state->ready.notify_one();
            }
        );
        std::unique_lock lock(state->mutex);
        state->ready.wait(lock, [&] { return state->value.has_value(); });
        return std::move(*state->value);
    }

    CallOptions options(int seconds = 5)
    {
        return {std::chrono::steady_clock::now() + std::chrono::seconds(seconds)};
    }

    /// Serves one caller-supplied response body for every request.
    class ScriptedTransport final : public HttpTransport
    {
      public:
        ScriptedTransport(std::shared_ptr<Runtime> runtime, std::string response, long status = 200)
            : runtime_(std::move(runtime)), response_(std::move(response)), status_(status)
        {
        }
        Task<Result<HttpResponse>> send(HttpRequest, CancellationToken) override
        {
            auto runtime = runtime_;
            auto body = response_;
            auto status = status_;
            return Task<Result<HttpResponse>>(
                [runtime, body, status](auto done) mutable
                {
                    detail_dispatch(
                        runtime,
                        [done = std::move(done), body, status]() mutable
                        {
                            done(HttpResponse{status, body, "application/soap+xml"});
                        }
                    );
                }
            );
        }

      private:
        static void detail_dispatch(const std::shared_ptr<Runtime>& runtime, std::function<void()> work)
        {
            if (!runtime->post(work))
            {
                work();
            }
        }
        std::shared_ptr<Runtime> runtime_;
        std::string response_;
        long status_;
    };

    /**
     * @brief Records every request body and answers a full PTZ/media/event device.
     *
     * Honours the cancellation token the way the real transport does: a request
     * carrying a cancelled token is refused before it reaches the wire, which is
     * what makes the safety-Stop regression observable.
     */
    class RecordingTransport final : public HttpTransport
    {
      public:
        explicit RecordingTransport(std::shared_ptr<Runtime> runtime)
            : runtime_(std::move(runtime))
        {
        }

        Task<Result<HttpResponse>> send(HttpRequest request, CancellationToken token) override
        {
            auto runtime = runtime_;
            const auto body = request.body;
            const bool cancelled = token.stop_requested();
            if (!cancelled)
            {
                std::lock_guard lock(mutex_);
                bodies.push_back(body);
            }
            return Task<Result<HttpResponse>>(
                [runtime, body, cancelled](auto done) mutable
                {
                    auto work = [done = std::move(done), body, cancelled]() mutable
                    {
                        if (cancelled)
                        {
                            done(Error{ErrorCategory::cancelled});
                            return;
                        }
                        done(HttpResponse{200, reply_for(body), "application/soap+xml"});
                    };
                    if (!runtime->post(work))
                    {
                        work();
                    }
                }
            );
        }

        std::size_t count(std::string_view needle)
        {
            std::lock_guard lock(mutex_);
            std::size_t total = 0;
            for (const auto& body : bodies)
            {
                if (body.find(needle) != std::string::npos)
                {
                    ++total;
                }
            }
            return total;
        }

        std::string last(std::string_view needle)
        {
            std::lock_guard lock(mutex_);
            for (auto entry = bodies.rbegin(); entry != bodies.rend(); ++entry)
            {
                if (entry->find(needle) != std::string::npos)
                {
                    return *entry;
                }
            }
            return {};
        }

        std::vector<std::string> bodies;

      private:
        // Answers with SOAP-ENV/ns2 prefixes, which is what real gSOAP devices emit.
        static std::string reply_for(const std::string& request)
        {
            const auto has = [&](std::string_view needle)
            {
                return request.find(needle) != std::string::npos;
            };
            std::string name;
            std::string body;
            if (has("GetCapabilities"))
            {
                name = "GetCapabilities";
                body = "<ns2:Media><ns2:XAddr>http://camera/media</ns2:XAddr></ns2:Media>"
                       "<ns2:PTZ><ns2:XAddr>http://camera/ptz</ns2:XAddr></ns2:PTZ>"
                       "<ns2:Events><ns2:XAddr>http://camera/events</ns2:XAddr></ns2:Events>";
            }
            else if (has("GetProfiles"))
            {
                name = "GetProfiles";
                // GuidToken must not satisfy a request for the "token" attribute.
                body = "<ns2:Profiles GuidToken=\"decoy-token\" token=\"main\" fixed=\"true\">"
                       "<ns2:Name>Main</ns2:Name>"
                       "<ns2:PTZConfiguration GuidToken=\"decoy\" token=\"cfg1\"/>"
                       "</ns2:Profiles>";
            }
            else if (has("GetConfigurationOptions"))
            {
                name = "GetConfigurationOptions";
                body = "<ns2:Spaces><ns2:ContinuousPanTiltVelocitySpace>"
                       "<ns2:XRange><ns2:Min>-4</ns2:Min><ns2:Max>4</ns2:Max></ns2:XRange>"
                       "<ns2:YRange><ns2:Min>-4</ns2:Min><ns2:Max>4</ns2:Max></ns2:YRange>"
                       "</ns2:ContinuousPanTiltVelocitySpace>"
                       "<ns2:ContinuousZoomVelocitySpace>"
                       "<ns2:XRange><ns2:Min>0</ns2:Min><ns2:Max>1</ns2:Max></ns2:XRange>"
                       "</ns2:ContinuousZoomVelocitySpace></ns2:Spaces>";
            }
            else if (has("GetPresets"))
            {
                name = "GetPresets";
                body = "<ns2:Preset token=\"p1\"><ns2:Name>Home</ns2:Name>"
                       "<ns2:PTZPosition><ns2:PanTilt x=\"2\" y=\"-4\"/>"
                       "<ns2:Zoom x=\"1\"/></ns2:PTZPosition></ns2:Preset>";
            }
            else if (has("RelativeMove")) { name = "RelativeMove"; }
            else if (has("ContinuousMove")) { name = "ContinuousMove"; }
            else if (has("tptz:Stop")) { name = "Stop"; }
            else if (has("CreatePullPointSubscription"))
            {
                name = "CreatePullPointSubscription";
                body = "<ns2:SubscriptionReference>"
                       "<ns2:Address>http://camera/subscription</ns2:Address>"
                       "</ns2:SubscriptionReference>";
            }
            else if (has("PullMessages"))
            {
                name = "PullMessages";
                body = "<ns2:NotificationMessage>"
                       "<ns2:Topic>tns1:RuleEngine/Motion</ns2:Topic>"
                       "<ns2:Message><ns2:Message UtcTime=\"2024-05-06T07:08:09Z\">"
                       "<ns2:Source><ns2:SimpleItem Name=\"Token\" Value=\"vs1\"/>"
                       "<ns2:SimpleItem Name=\"Rule\" Value=\"r7\"/></ns2:Source>"
                       "<ns2:Data><ns2:SimpleItem Name=\"State\" Value=\"true\"/></ns2:Data>"
                       "</ns2:Message></ns2:Message></ns2:NotificationMessage>";
            }
            else if (has("Renew")) { name = "Renew"; }
            else if (has("Unsubscribe")) { name = "Unsubscribe"; }
            else { name = "Unknown"; }
            return "<SOAP-ENV:Envelope "
                   "xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\" "
                   "xmlns:ns2=\"http://www.onvif.org/ver10/schema\">"
                   "<SOAP-ENV:Header><ns2:Address>http://decoy/header</ns2:Address>"
                   "</SOAP-ENV:Header><SOAP-ENV:Body><ns2:" +
                   name + "Response>" + body + "</ns2:" + name +
                   "Response></SOAP-ENV:Body></SOAP-ENV:Envelope>";
        }

        std::mutex mutex_;
        std::shared_ptr<Runtime> runtime_;
    };

    std::string envelope(std::string_view body, std::string_view header = {})
    {
        return "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\" "
               "xmlns:ns2=\"http://www.onvif.org/ver10/device/wsdl\">"
               "<SOAP-ENV:Header>" +
               std::string(header) + "</SOAP-ENV:Header><SOAP-ENV:Body>" + std::string(body) +
               "</SOAP-ENV:Body></SOAP-ENV:Envelope>";
    }
} // namespace

#define CHECK(expression)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (!(expression))                                                                         \
        {                                                                                          \
            std::cerr << "regression failed: " #expression " at line " << __LINE__ << '\n';        \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

int main()
{
    // A non-C global locale must not reach any serialized number. Absent the
    // locale, the precision assertions below still catch the same defect.
    try
    {
        std::locale::global(std::locale("de_DE.UTF-8"));
    }
    catch (...)
    {
        try
        {
            std::locale::global(std::locale("de_DE.utf8"));
        }
        catch (...)
        {
            std::cerr << "note: de_DE locale unavailable; locale assertions are weaker\n";
        }
    }

    auto runtime_result = Runtime::create();
    CHECK(runtime_result);
    if (!runtime_result)
    {
        return 1;
    }
    auto runtime = runtime_result.value();

    // ---- read_soap_operation collects only the response element's own text ----
    {
        const auto scoped = read_soap_operation(
            envelope(
                "<ns2:GetDeviceInformationResponse><ns2:Manufacturer>ACME"
                "</ns2:Manufacturer></ns2:GetDeviceInformationResponse>"
                "<ns2:Trailing>LEAKED-SIBLING</ns2:Trailing>",
                "<ns2:Decoy>LEAKED-HEADER</ns2:Decoy>"
            ),
            "GetDeviceInformationResponse"
        );
        CHECK(scoped);
        CHECK(scoped && scoped.value().find("ACME") != std::string::npos);
        CHECK(scoped && scoped.value().find("LEAKED-SIBLING") == std::string::npos);
        CHECK(scoped && scoped.value().find("LEAKED-HEADER") == std::string::npos);
    }

    // ---- Device decoding ignores the namespace prefix ----
    {
        const auto check_information = [&](const std::string& response, const char* label)
        {
            auto transport = std::make_shared<ScriptedTransport>(runtime, response);
            auto session = sync(DeviceSession::connect(
                "http://camera/onvif/device_service",
                transport,
                make_same_host_policy(),
                runtime,
                std::nullopt,
                options()
            ));
            CHECK(session);
            if (!session)
            {
                return;
            }
            auto info = sync(session.value()->get_device_information(options()));
            CHECK(info);
            if (!info)
            {
                std::cerr << "  (" << label << ")\n";
                return;
            }
            CHECK(info.value().manufacturer == "ACME");
            CHECK(info.value().model == "CAM-1");
            CHECK(info.value().serial_number == "SN9");
        };
        // gSOAP-style prefix, which is what the reference cameras emit.
        check_information(
            envelope("<ns2:GetDeviceInformationResponse>"
                     "<ns2:Manufacturer>ACME</ns2:Manufacturer><ns2:Model>CAM-1</ns2:Model>"
                     "<ns2:FirmwareVersion>1.2</ns2:FirmwareVersion>"
                     "<ns2:SerialNumber>SN9</ns2:SerialNumber>"
                     "<ns2:HardwareId>HW3</ns2:HardwareId></ns2:GetDeviceInformationResponse>"),
            "ns2 prefix"
        );
        // Default namespace, no prefix at all.
        check_information(
            "<Envelope xmlns=\"http://www.w3.org/2003/05/soap-envelope\"><Body>"
            "<GetDeviceInformationResponse xmlns=\"http://www.onvif.org/ver10/device/wsdl\">"
            "<Manufacturer>ACME</Manufacturer><Model>CAM-1</Model>"
            "<FirmwareVersion>1.2</FirmwareVersion><SerialNumber>SN9</SerialNumber>"
            "<HardwareId>HW3</HardwareId></GetDeviceInformationResponse></Body></Envelope>",
            "default namespace"
        );
        // A decoy in the SOAP Header must never win over the real response field.
        check_information(
            envelope(
                "<ns2:GetDeviceInformationResponse>"
                "<ns2:Manufacturer>ACME</ns2:Manufacturer><ns2:Model>CAM-1</ns2:Model>"
                "<ns2:FirmwareVersion>1.2</ns2:FirmwareVersion>"
                "<ns2:SerialNumber>SN9</ns2:SerialNumber>"
                "<ns2:HardwareId>HW3</ns2:HardwareId></ns2:GetDeviceInformationResponse>",
                "<ns2:Manufacturer>HEADER-DECOY</ns2:Manufacturer>"
                "<ns2:SerialNumber>HEADER-DECOY</ns2:SerialNumber>"
            ),
            "header decoy"
        );
    }

    // ---- GetSystemDateAndTime reports failure instead of the epoch ----
    {
        const auto time_of = [&](const std::string& payload)
        {
            auto transport = std::make_shared<ScriptedTransport>(
                runtime,
                envelope("<ns2:GetSystemDateAndTimeResponse>" + payload +
                         "</ns2:GetSystemDateAndTimeResponse>")
            );
            auto session = sync(DeviceSession::connect(
                "http://camera/onvif/device_service",
                transport,
                make_same_host_policy(),
                runtime,
                std::nullopt,
                options()
            ));
            return sync(session.value()->get_system_date_and_time(options()));
        };
        auto absent = time_of("<ns2:SystemDateAndTime/>");
        CHECK(!absent);
        CHECK(!absent && absent.error().category == ErrorCategory::protocol_violation);

        auto invalid = time_of(
            "<ns2:UTCDateTime><ns2:Time><ns2:Hour>99</ns2:Hour><ns2:Minute>0</ns2:Minute>"
            "<ns2:Second>0</ns2:Second></ns2:Time><ns2:Date><ns2:Year>2024</ns2:Year>"
            "<ns2:Month>13</ns2:Month><ns2:Day>40</ns2:Day></ns2:Date></ns2:UTCDateTime>"
        );
        CHECK(!invalid);
        CHECK(!invalid && invalid.error().category == ErrorCategory::protocol_violation);

        // LocalDateTime is emitted first and must not be mistaken for UTC.
        auto utc = time_of(
            "<ns2:LocalDateTime><ns2:Time><ns2:Hour>23</ns2:Hour><ns2:Minute>0</ns2:Minute>"
            "<ns2:Second>0</ns2:Second></ns2:Time><ns2:Date><ns2:Year>1999</ns2:Year>"
            "<ns2:Month>12</ns2:Month><ns2:Day>31</ns2:Day></ns2:Date></ns2:LocalDateTime>"
            "<ns2:UTCDateTime><ns2:Time><ns2:Hour>7</ns2:Hour><ns2:Minute>8</ns2:Minute>"
            "<ns2:Second>9</ns2:Second></ns2:Time><ns2:Date><ns2:Year>2024</ns2:Year>"
            "<ns2:Month>5</ns2:Month><ns2:Day>6</ns2:Day></ns2:Date></ns2:UTCDateTime>"
        );
        CHECK(utc);
        CHECK(utc && std::chrono::duration_cast<std::chrono::seconds>(
                         utc.value().utc.time_since_epoch()
                     )
                             .count() == 1714979289);
    }

    // ---- Capabilities read each XAddr from inside its own service element ----
    {
        // Media advertises no XAddr; the PTZ address must not be adopted for it.
        auto transport = std::make_shared<ScriptedTransport>(
            runtime,
            envelope("<ns2:GetCapabilitiesResponse><ns2:Capabilities>"
                     "<ns2:Media><ns2:StreamingCapabilities/></ns2:Media>"
                     "<ns2:PTZ><ns2:XAddr>http://camera/ptz</ns2:XAddr></ns2:PTZ>"
                     "<ns2:Events><ns2:XAddr>http://camera/events</ns2:XAddr></ns2:Events>"
                     "</ns2:Capabilities></ns2:GetCapabilitiesResponse>")
        );
        auto session = sync(DeviceSession::connect(
            "http://camera/onvif/device_service",
            transport,
            make_same_host_policy(),
            runtime,
            std::nullopt,
            options()
        ));
        auto capabilities = sync(session.value()->get_capabilities(options()));
        CHECK(capabilities);
        CHECK(capabilities && !capabilities.value().media_xaddr);
        CHECK(capabilities && capabilities.value().ptz_xaddr == "http://camera/ptz");
        CHECK(capabilities && capabilities.value().events_xaddr == "http://camera/events");
    }

    // ---- Scopes and services decode under a foreign prefix ----
    {
        auto transport = std::make_shared<ScriptedTransport>(
            runtime,
            envelope("<ns2:GetScopesResponse>"
                     "<ns2:Scopes><ns2:ScopeItem>onvif://www.onvif.org/name/A</ns2:ScopeItem></ns2:Scopes>"
                     "<ns2:Scopes><ns2:ScopeItem>onvif://www.onvif.org/name/B</ns2:ScopeItem></ns2:Scopes>"
                     "</ns2:GetScopesResponse>")
        );
        auto session = sync(DeviceSession::connect(
            "http://camera/onvif/device_service",
            transport,
            make_same_host_policy(),
            runtime,
            std::nullopt,
            options()
        ));
        auto scopes = sync(session.value()->get_scopes(options()));
        CHECK(scopes);
        CHECK(scopes && scopes.value().size() == 2);
        CHECK(scopes && scopes.value().size() == 2 &&
              scopes.value()[1] == "onvif://www.onvif.org/name/B");
    }

    // ---- Network backend: PTZ serialization, presets, events, safety Stop ----
    {
        auto transport = std::make_shared<RecordingTransport>(runtime);
        auto session = sync(DeviceSession::connect(
            "http://camera/onvif/device_service",
            transport,
            make_same_host_policy(),
            runtime,
            std::nullopt,
            options()
        ));
        CHECK(session);
        auto ptz = session.value()->ptz();
        CHECK(ptz);

        // Numbers must be locale-independent and round-trippable, not stream-formatted.
        auto moved = sync(ptz.value()->relative_move("main", {0.123456789012345, {}, {}}, options()));
        CHECK(moved);
        const auto move_body = transport->last("RelativeMove");
        CHECK(move_body.find(',') == std::string::npos);
        CHECK(move_body.find("0.4938271560493801") != std::string::npos);

        // Preset positions are published normalized, not in native units.
        auto presets = sync(ptz.value()->get_presets("main", options()));
        CHECK(presets);
        CHECK(presets && presets.value().size() == 1);
        if (presets && presets.value().size() == 1)
        {
            const auto& preset = presets.value()[0];
            CHECK(preset.token == "p1");
            CHECK(preset.position.pan && std::abs(*preset.position.pan - 0.5) < 1e-12);
            CHECK(preset.position.tilt && std::abs(*preset.position.tilt + 1.0) < 1e-12);
            CHECK(preset.position.zoom && std::abs(*preset.position.zoom - 1.0) < 1e-12);
        }

        // Profiles: "token" must not be satisfied by a "GuidToken" attribute.
        auto media = session.value()->media();
        auto profiles = sync(media.value()->get_profiles(options()));
        CHECK(profiles);
        CHECK(profiles && profiles.value().size() == 1);
        CHECK(profiles && profiles.value().size() == 1 && profiles.value()[0].token == "main");

        // Events: Source and Data are split by subtree and the device clock is used.
        auto stream = session.value()->events(8);
        CHECK(bool(sync(stream.value()->poll(4, options()))));
        auto item = sync(stream.value()->next(options()));
        CHECK(item && std::holds_alternative<Event>(item.value()));
        if (item && std::holds_alternative<Event>(item.value()))
        {
            const auto& event = std::get<Event>(item.value());
            CHECK(event.topic == "tns1:RuleEngine/Motion");
            CHECK(event.source == "Token=vs1,Rule=r7");
            CHECK(event.data == "State=true");
            CHECK(std::chrono::duration_cast<std::chrono::seconds>(
                      event.utc_time.time_since_epoch()
                  )
                      .count() == 1714979289);
        }

        // One backend per session: a second events() must not resubscribe.
        auto second = session.value()->events(8);
        CHECK(bool(sync(second.value()->poll(4, options()))));
        CHECK(transport->count("CreatePullPointSubscription") == 1);

        // A cancelled movement must still deliver its safety Stop. The transport
        // refuses a request carrying a cancelled token, so a Stop issued under the
        // caller's own token never reaches the device.
        const auto stops_before = transport->count("tptz:Stop");
        CancellationSource cancelled;
        cancelled.request_stop();
        auto refused = sync(
            ptz.value()->relative_move("main", {0.1, {}, {}}, {options().deadline, cancelled.token()})
        );
        CHECK(!refused);
        CHECK(!refused && refused.error().category == ErrorCategory::cancelled);
        CHECK(transport->count("tptz:Stop") == stops_before + 1);

        // An emergency Stop must also survive an already-cancelled token.
        const auto emergency_before = transport->count("tptz:Stop");
        auto emergency = sync(
            ptz.value()->emergency_stop("main", {options().deadline, cancelled.token()})
        );
        CHECK(emergency);
        CHECK(transport->count("tptz:Stop") == emergency_before + 1);
    }

    // ---- Managed event stream honours its bound and always reports loss ----
    {
        auto device = std::make_shared<VirtualDeviceTransport>(
            runtime,
            DeviceInformation{},
            std::vector<std::string>{},
            std::vector<Service>{},
            Capabilities{}
        );
        ManagedEventStream stream(runtime, device, 2);
        for (int index = 0; index < 6; ++index)
        {
            Event event;
            event.topic = "topic/" + std::to_string(index);
            device->emit(event);
        }
        CHECK(bool(sync(stream.poll(6, options()))));
        std::size_t delivered = 0;
        bool reported_loss = false;
        for (;;)
        {
            auto item = sync(stream.next(options()));
            if (!item)
            {
                break;
            }
            ++delivered;
            if (std::holds_alternative<StreamHealth>(item.value()) &&
                std::get<StreamHealth>(item.value()) == StreamHealth::events_dropped)
            {
                reported_loss = true;
            }
        }
        CHECK(delivered <= 2);
        CHECK(reported_loss);

        // Evicting an event to make room for a reconnect marker is still a loss.
        ManagedEventStream second(runtime, device, 2);
        Event first_event;
        first_event.topic = "a";
        Event second_event;
        second_event.topic = "b";
        device->emit(first_event);
        device->emit(second_event);
        CHECK(bool(sync(second.poll(2, options()))));
        CHECK(bool(sync(second.mark_reconnected(options()))));
        bool second_reported_loss = false;
        std::size_t second_delivered = 0;
        for (;;)
        {
            auto item = sync(second.next(options()));
            if (!item)
            {
                break;
            }
            ++second_delivered;
            if (std::holds_alternative<StreamHealth>(item.value()) &&
                std::get<StreamHealth>(item.value()) == StreamHealth::events_dropped)
            {
                second_reported_loss = true;
            }
        }
        CHECK(second_delivered <= 2);
        CHECK(second_reported_loss);
    }

    // ---- The virtual device advertises its Events capability ----
    {
        Capabilities capabilities;
        capabilities.media_xaddr = "http://camera/media";
        capabilities.ptz_xaddr = "http://camera/ptz";
        capabilities.events_xaddr = "http://camera/events";
        auto device = std::make_shared<VirtualDeviceTransport>(
            runtime,
            DeviceInformation{},
            std::vector<std::string>{},
            std::vector<Service>{},
            capabilities
        );
        auto session = sync(DeviceSession::connect(
            "http://camera/onvif/device_service",
            device,
            make_same_host_policy(),
            runtime,
            std::nullopt,
            options()
        ));
        auto reported = sync(session.value()->get_capabilities(options()));
        CHECK(reported);
        CHECK(reported && reported.value().events_xaddr == "http://camera/events");
        // The virtual device must also report a usable clock.
        auto now = sync(session.value()->get_system_date_and_time(options()));
        CHECK(now);
        CHECK(now && now.value().utc.time_since_epoch().count() > 0);
    }

    // ---- A URL without a scheme must not authorize itself ----
    {
        auto same_origin = make_same_origin_policy();
        CHECK(!same_origin->authorize("camera/onvif", "camera/onvif"));
        CHECK(same_origin->authorize("http://camera/onvif", "http://camera"));
        // Origin comparison is case-insensitive on scheme and host.
        CHECK(same_origin->authorize("HTTP://CAMERA/onvif", "http://camera"));
    }

    // ---- Deduplication keys stay bounded and still deduplicate ----
    {
        DiscoverySession discovery(runtime, 4);
        DiscoveryObservation observation{
            DiscoveryKind::probe_match, "urn:uuid:1", "192.0.2.1", "eth0", {}, {}, 1
        };
        CHECK(bool(sync(discovery.inject(observation, options()))));
        CHECK(bool(sync(discovery.inject(observation, options()))));
        // Far more distinct keys than the queue bound must not disturb correctness.
        for (std::uint64_t index = 0; index < 4000; ++index)
        {
            DiscoveryObservation unique = observation;
            unique.message_number = index + 100;
            (void)sync(discovery.inject(unique, options()));
        }
        auto first = sync(discovery.next(options()));
        CHECK(first);
        CHECK(first && first.value().endpoint_reference == "urn:uuid:1");
        CHECK(bool(sync(discovery.close(options()))));
    }

    // ---- A completion still runs when the runtime refuses new work ----
    {
        auto stopping = Runtime::create().value();
        CHECK(bool(stopping->shutdown()));
        std::atomic_bool called{false};
        auto device = std::make_shared<VirtualDeviceTransport>(
            stopping,
            DeviceInformation{},
            std::vector<std::string>{},
            std::vector<Service>{},
            Capabilities{}
        );
        auto task = DeviceSession::connect(
            "http://camera/onvif/device_service",
            device,
            make_same_host_policy(),
            stopping,
            std::nullopt,
            options()
        );
        std::move(task).start([&](Result<std::shared_ptr<DeviceSession>>) { called = true; });
        CHECK(called.load());
    }

#ifndef _WIN32
    // ---- A spoofed ProbeMatch cannot redirect the caller to a foreign host ----
    {
        int server = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(3702);
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (server >= 0 &&
            bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
        {
            std::thread responder(
                [server]
                {
                    std::vector<char> buffer(65536);
                    sockaddr_in peer{};
                    socklen_t peer_size = sizeof(peer);
                    const auto received = recvfrom(
                        server,
                        buffer.data(),
                        buffer.size(),
                        0,
                        reinterpret_cast<sockaddr*>(&peer),
                        &peer_size
                    );
                    if (received <= 0)
                    {
                        return;
                    }
                    const std::string reply =
                        "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
                        "xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
                        "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\">"
                        "<e:Header><w:MessageID>urn:uuid:spoof</w:MessageID></e:Header><e:Body>"
                        "<d:ProbeMatches><d:ProbeMatch><w:EndpointReference>"
                        "<w:Address>urn:uuid:spoofed</w:Address></w:EndpointReference>"
                        "<d:XAddrs>http://169.254.169.254/latest/meta-data</d:XAddrs>"
                        "</d:ProbeMatch></d:ProbeMatches></e:Body></e:Envelope>";
                    sendto(
                        server,
                        reply.data(),
                        reply.size(),
                        0,
                        reinterpret_cast<sockaddr*>(&peer),
                        peer_size
                    );
                }
            );
            NetworkDiscoveryOptions discovery;
            discovery.multicast = false;
            discovery.direct_probe = false;
            discovery.retransmissions = 1;
            discovery.listen_duration = std::chrono::milliseconds(700);
            discovery.targets = {"127.0.0.1"};
            auto found = sync(
                discover_devices(runtime, make_same_host_policy(), discovery, options(15))
            );
            responder.join();
            close(server);
            CHECK(found);
            if (found && found.value().size() == 1)
            {
                const auto& candidate = found.value()[0];
                // The foreign address must not become a connectable endpoint...
                CHECK(!candidate.device_service);
                // ...while the raw advertisement is still reported as untrusted data.
                CHECK(candidate.discovery && candidate.discovery->xaddrs.size() == 1);
            }
            else
            {
                std::cerr << "note: no ProbeMatch observed; spoofing check skipped\n";
            }
        }
        else
        {
            if (server >= 0)
            {
                close(server);
            }
            std::cerr << "note: cannot bind 127.0.0.1:3702; spoofing check skipped\n";
        }
    }
#endif

    CHECK(bool(runtime->shutdown()));
    if (failures != 0)
    {
        std::cerr << failures << " regression check(s) failed\n";
        return 1;
    }
    std::cout << "regression tests passed\n";
    return 0;
}

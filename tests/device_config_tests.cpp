/**
 * @file device_config_tests.cpp
 * @brief Clock, NTP, and scope configuration against virtual and recorded devices.
 *
 * The recorded fixtures are replies from a consumer PTZ camera with gSOAP firmware, whose
 * `SOAP-ENV:`/`tds:`/`tt:` prefixes and fault shapes differ from the virtual
 * device's, so decoders cannot pass by matching only one style.
 */

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "recorded_camera_transport.hpp"

using namespace wlonvif;
using namespace std::chrono_literals;

namespace
{
    int failures = 0;

#define CHECK(expression)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (!(expression))                                                                         \
        {                                                                                          \
            std::cerr << "device config check failed: " #expression " at line " << __LINE__     \
                      << '\n';                                                                     \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

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

    CallOptions options()
    {
        return {std::chrono::steady_clock::now() + 5s};
    }

    /// Forwards to a device while recording every request body.
    class RecordingTransport final : public HttpTransport
    {
      public:
        explicit RecordingTransport(std::shared_ptr<HttpTransport> device)
            : device_(std::move(device)) {}
        Task<Result<HttpResponse>> send(HttpRequest request, CancellationToken token) override
        {
            {
                std::lock_guard lock(mutex_);
                bodies_.push_back(request.body);
            }
            return device_->send(std::move(request), std::move(token));
        }
        std::size_t count(std::string_view needle)
        {
            std::lock_guard lock(mutex_);
            std::size_t total = 0;
            for (const auto& body : bodies_) total += body.find(needle) != std::string::npos;
            return total;
        }
        std::string last(std::string_view needle)
        {
            std::lock_guard lock(mutex_);
            for (auto body = bodies_.rbegin(); body != bodies_.rend(); ++body)
                if (body->find(needle) != std::string::npos) return *body;
            return {};
        }

      private:
        std::shared_ptr<HttpTransport> device_;
        std::mutex mutex_;
        std::vector<std::string> bodies_;
    };

    /// Replies with fixed bodies chosen by the operation named in the request.
    class FixtureTransport final : public HttpTransport
    {
      public:
        void reply(std::string operation, long status, std::string body)
        {
            replies_.push_back({std::move(operation), status, std::move(body)});
        }
        Task<Result<HttpResponse>> send(HttpRequest request, CancellationToken) override
        {
            for (const auto& reply : replies_)
            {
                if (request.body.find(reply.operation) == std::string::npos) continue;
                HttpResponse response{reply.status, reply.body, "application/soap+xml"};
                return Task<Result<HttpResponse>>([response](auto done) mutable
                                                  { done(std::move(response)); });
            }
            return Task<Result<HttpResponse>>([](auto done)
                                              { done(Error{ErrorCategory::unsupported}); });
        }

      private:
        struct Reply
        {
            std::string operation;
            long status;
            std::string body;
        };
        std::vector<Reply> replies_;
    };

    std::string gsoap_envelope(std::string_view body)
    {
        return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><SOAP-ENV:Envelope "
               "xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\" "
               "xmlns:tt=\"http://www.onvif.org/ver10/schema\" "
               "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
               "xmlns:ter=\"http://www.onvif.org/ver10/error\"><SOAP-ENV:Body>" +
               std::string(body) + "</SOAP-ENV:Body></SOAP-ENV:Envelope>";
    }

    /// Return the UsernameToken Created time in @p body minus the real clock.
    std::optional<std::chrono::seconds> created_skew(const std::string& body)
    {
        const auto start = body.find("<wsu:Created>");
        if (start == std::string::npos) return std::nullopt;
        int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
        if (std::sscanf(body.c_str() + start + 13, "%4d-%2d-%2dT%2d:%2d:%2dZ",
                        &year, &month, &day, &hour, &minute, &second) != 6)
            return std::nullopt;
        const auto created = std::chrono::sys_days(
            std::chrono::year(year) / std::chrono::month(static_cast<unsigned>(month)) /
            std::chrono::day(static_cast<unsigned>(day))
        ) + std::chrono::hours(hour) + std::chrono::minutes(minute) + std::chrono::seconds(second);
        return std::chrono::round<std::chrono::seconds>(created - std::chrono::system_clock::now());
    }

    std::shared_ptr<DeviceSession> connect(
        const std::shared_ptr<Runtime>& runtime,
        std::shared_ptr<HttpTransport> transport,
        std::optional<Credentials> credentials = std::nullopt
    )
    {
        auto session = sync(DeviceSession::connect(
            "http://camera/onvif/device_service", std::move(transport), make_same_host_policy(),
            runtime, std::move(credentials), options()
        ));
        CHECK(session);
        return session ? session.value() : nullptr;
    }

    std::shared_ptr<VirtualDeviceTransport> virtual_device(const std::shared_ptr<Runtime>& runtime)
    {
        return std::make_shared<VirtualDeviceTransport>(
            runtime,
            DeviceInformation{"Example", "Camera", "1.0", "0001", "hw"},
            std::vector<std::string>{
                "onvif://www.onvif.org/name/Camera", "onvif://www.onvif.org/location/Hong%20Kong"
            },
            std::vector<Service>{},
            Capabilities{}
        );
    }

    bool has_scope(const std::vector<ScopeEntry>& scopes, std::string_view uri, ScopeDefinition definition)
    {
        for (const auto& scope : scopes)
            if (scope.uri == uri && scope.definition == definition) return true;
        return false;
    }
} // namespace

int main()
{
    auto runtime = Runtime::create().value();

    // ---- Scopes: fixed scopes are protected, configurable ones round-trip ----
    {
        auto session = connect(runtime, virtual_device(runtime));
        auto scopes = sync(session->get_scope_entries(options()));
        CHECK(scopes && scopes.value().size() == 2);
        CHECK(scopes && has_scope(scopes.value(), "onvif://www.onvif.org/location/Hong%20Kong",
                                  ScopeDefinition::fixed));

        CHECK(sync(session->add_scopes({"onvif://www.onvif.org/location/Lab"}, options())));
        scopes = sync(session->get_scope_entries(options()));
        CHECK(scopes && has_scope(scopes.value(), "onvif://www.onvif.org/location/Lab",
                                  ScopeDefinition::configurable));
        // The plain list still carries every scope, fixed and configurable.
        auto plain = sync(session->get_scopes(options()));
        CHECK(plain && plain.value().size() == 3);

        auto overwrite = sync(session->set_scopes({"onvif://www.onvif.org/location/Hong%20Kong"}, options()));
        CHECK(!overwrite && overwrite.error().category == ErrorCategory::soap_fault);
        CHECK(!overwrite && overwrite.error().soap_fault &&
              overwrite.error().soap_fault->subcode == "ter:ScopeOverwrite");

        CHECK(sync(session->set_scopes({"onvif://www.onvif.org/location/Office"}, options())));
        scopes = sync(session->get_scope_entries(options()));
        CHECK(scopes && scopes.value().size() == 3); // Lab replaced, fixed scopes kept.
        CHECK(scopes && has_scope(scopes.value(), "onvif://www.onvif.org/location/Office",
                                  ScopeDefinition::configurable));

        auto fixed = sync(session->remove_scopes({"onvif://www.onvif.org/name/Camera"}, options()));
        CHECK(!fixed && fixed.error().soap_fault &&
              fixed.error().soap_fault->subcode == "ter:FixedScope");
        auto missing = sync(session->remove_scopes({"onvif://www.onvif.org/location/Nowhere"}, options()));
        CHECK(!missing && missing.error().soap_fault &&
              missing.error().soap_fault->subcode == "ter:NoScope");
        CHECK(sync(session->remove_scopes({"onvif://www.onvif.org/location/Office"}, options())));
        scopes = sync(session->get_scope_entries(options()));
        CHECK(scopes && scopes.value().size() == 2);

        // Invalid scope lists are refused before anything is sent.
        auto empty = sync(session->add_scopes({}, options()));
        CHECK(!empty && empty.error().category == ErrorCategory::invalid_argument);
        auto spaced = sync(session->add_scopes({"onvif://www.onvif.org/location/two words"}, options()));
        CHECK(!spaced && spaced.error().category == ErrorCategory::invalid_argument);
        // Markup in a scope is escaped, not interpreted.
        CHECK(sync(session->add_scopes({"onvif://www.onvif.org/location/a<b>&c"}, options())));
        scopes = sync(session->get_scope_entries(options()));
        CHECK(scopes && has_scope(scopes.value(), "onvif://www.onvif.org/location/a<b>&c",
                                  ScopeDefinition::configurable));
    }

    // ---- Clock and NTP settings round-trip ----
    {
        auto session = connect(runtime, virtual_device(runtime));
        SystemDateAndTimeSettings manual;
        manual.type = DateTimeType::manual;
        manual.daylight_savings = true;
        manual.time_zone = "CET-1CEST,M3.5.0,M10.5.0/3";
        manual.utc = std::chrono::system_clock::now() - 2h;
        CHECK(sync(session->set_system_date_and_time(manual, options())));
        auto clock = sync(session->get_system_date_and_time(options()));
        CHECK(clock && clock.value().type == DateTimeType::manual);
        CHECK(clock && clock.value().daylight_savings);
        CHECK(clock && clock.value().time_zone == "CET-1CEST,M3.5.0,M10.5.0/3");
        CHECK(clock && std::chrono::abs(clock.value().utc - (std::chrono::system_clock::now() - 2h)) < 5s);

        // Omitting the time zone leaves it unchanged.
        SystemDateAndTimeSettings ntp;
        ntp.type = DateTimeType::ntp;
        CHECK(sync(session->set_system_date_and_time(ntp, options())));
        clock = sync(session->get_system_date_and_time(options()));
        CHECK(clock && clock.value().type == DateTimeType::ntp && !clock.value().daylight_savings);
        CHECK(clock && clock.value().time_zone == "CET-1CEST,M3.5.0,M10.5.0/3");
        CHECK(clock && std::chrono::abs(clock.value().utc - std::chrono::system_clock::now()) < 5s);

        auto no_time = manual;
        no_time.utc.reset();
        auto refused = sync(session->set_system_date_and_time(no_time, options()));
        CHECK(!refused && refused.error().category == ErrorCategory::invalid_argument);
        auto ntp_time = ntp;
        ntp_time.utc = std::chrono::system_clock::now();
        refused = sync(session->set_system_date_and_time(ntp_time, options()));
        CHECK(!refused && refused.error().category == ErrorCategory::invalid_argument);
        auto zone = ntp;
        zone.time_zone = "";
        refused = sync(session->set_system_date_and_time(zone, options()));
        CHECK(!refused && refused.error().category == ErrorCategory::invalid_argument);

        CHECK(sync(session->set_ntp({false, {"pool.ntp.org", "192.0.2.1"}, {}}, options())));
        auto configured = sync(session->get_ntp(options()));
        CHECK(configured && !configured.value().from_dhcp);
        CHECK(configured && configured.value().servers ==
                                std::vector<std::string>({"pool.ntp.org", "192.0.2.1"}));
        CHECK(sync(session->set_ntp({true, {}, {}}, options())));
        configured = sync(session->get_ntp(options()));
        CHECK(configured && configured.value().from_dhcp && configured.value().servers.empty());
        auto contradictory = sync(session->set_ntp({true, {"pool.ntp.org"}, {}}, options()));
        CHECK(!contradictory && contradictory.error().category == ErrorCategory::invalid_argument);
        auto none = sync(session->set_ntp({false, {}, {}}, options()));
        CHECK(!none && none.error().category == ErrorCategory::invalid_argument);
    }

    // ---- Changing the clock re-measures it for signed requests ----
    {
        auto recording = std::make_shared<RecordingTransport>(virtual_device(runtime));
        auto session = connect(runtime, recording, Credentials{"admin", "secret"});
        CHECK(sync(session->get_device_information(options())));
        CHECK(recording->count("GetSystemDateAndTime") == 1);
        SystemDateAndTimeSettings manual;
        manual.type = DateTimeType::manual;
        manual.utc = std::chrono::system_clock::now() - 5h;
        CHECK(sync(session->set_system_date_and_time(manual, options())));
        CHECK(sync(session->get_scope_entries(options())));
        CHECK(recording->count("GetSystemDateAndTime") == 2);
        const auto skew = created_skew(recording->last("GetScopes"));
        CHECK(skew && std::chrono::abs(*skew + 5h) <= 3s);
    }

    // ---- Replies recorded from a consumer PTZ camera ----
    {
        auto device = std::make_shared<FixtureTransport>();
        device->reply("GetScopes", 200, gsoap_envelope(
            "<tds:GetScopesResponse><tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef>"
            "<tt:ScopeItem>onvif://www.onvif.org/name/Camera</tt:ScopeItem></tds:Scopes>"
            "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef>"
            "<tt:ScopeItem>onvif://www.onvif.org/location/Hong%20Kong</tt:ScopeItem></tds:Scopes>"
            "<tds:Scopes><tt:ScopeDef>Configurable</tt:ScopeDef>"
            "<tt:ScopeItem>onvif://www.onvif.org/location/Office%202</tt:ScopeItem></tds:Scopes>"
            "</tds:GetScopesResponse>"));
        device->reply("GetNTP", 200, gsoap_envelope(
            "<tds:GetNTPResponse><tds:NTPInformation><tt:FromDHCP>true</tt:FromDHCP>"
            "<tt:NTPFromDHCP><tt:Type>IPv4</tt:Type><tt:IPv4Address>0.0.0.0</tt:IPv4Address>"
            "</tt:NTPFromDHCP></tds:NTPInformation></tds:GetNTPResponse>"));
        device->reply("GetSystemDateAndTime", 200, gsoap_envelope(
            "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime>"
            "<tt:DateTimeType>NTP</tt:DateTimeType><tt:DaylightSavings>true</tt:DaylightSavings>"
            "<tt:TimeZone><tt:TZ>GMT+00:00</tt:TZ></tt:TimeZone><tt:UTCDateTime><tt:Time>"
            "<tt:Hour>10</tt:Hour><tt:Minute>49</tt:Minute><tt:Second>44</tt:Second></tt:Time>"
            "<tt:Date><tt:Year>2026</tt:Year><tt:Month>10</tt:Month><tt:Day>20</tt:Day></tt:Date>"
            "</tt:UTCDateTime><tt:LocalDateTime><tt:Time><tt:Hour>11</tt:Hour>"
            "<tt:Minute>49</tt:Minute><tt:Second>44</tt:Second></tt:Time><tt:Date>"
            "<tt:Year>2026</tt:Year><tt:Month>10</tt:Month><tt:Day>20</tt:Day></tt:Date>"
            "</tt:LocalDateTime></tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>"));
        device->reply("SetScopes", 400, gsoap_envelope(
            "<SOAP-ENV:Fault><SOAP-ENV:Code><SOAP-ENV:Value>SOAP-ENV:Sender</SOAP-ENV:Value>"
            "<SOAP-ENV:Subcode><SOAP-ENV:Value>ter:OperationProhibited</SOAP-ENV:Value>"
            "<SOAP-ENV:Subcode><SOAP-ENV:Value>ter:ScopeOverwrite</SOAP-ENV:Value>"
            "</SOAP-ENV:Subcode></SOAP-ENV:Subcode></SOAP-ENV:Code><SOAP-ENV:Reason>"
            "<SOAP-ENV:Text xml:lang=\"en\"></SOAP-ENV:Text></SOAP-ENV:Reason></SOAP-ENV:Fault>"));
        device->reply("RemoveScopes", 200, gsoap_envelope(
            "<tds:RemoveScopesResponse><tds:ScopeItem>onvif://www.onvif.org/location/Office%202"
            "</tds:ScopeItem></tds:RemoveScopesResponse>"));
        device->reply("SetNTP", 200, gsoap_envelope("<tds:SetNTPResponse></tds:SetNTPResponse>"));
        auto session = connect(runtime, device);

        auto scopes = sync(session->get_scope_entries(options()));
        CHECK(scopes && scopes.value().size() == 3);
        CHECK(scopes && has_scope(scopes.value(), "onvif://www.onvif.org/location/Hong%20Kong",
                                  ScopeDefinition::fixed));
        CHECK(scopes && has_scope(scopes.value(), "onvif://www.onvif.org/location/Office%202",
                                  ScopeDefinition::configurable));

        auto ntp = sync(session->get_ntp(options()));
        CHECK(ntp && ntp.value().from_dhcp && ntp.value().servers.empty());
        CHECK(ntp && ntp.value().dhcp_servers == std::vector<std::string>{"0.0.0.0"});

        auto clock = sync(session->get_system_date_and_time(options()));
        CHECK(clock && clock.value().type == DateTimeType::ntp);
        CHECK(clock && clock.value().daylight_savings && clock.value().time_zone == "GMT+00:00");
        // UTCDateTime, not the decoy LocalDateTime an hour later.
        CHECK(clock && clock.value().utc ==
                           std::chrono::sys_days(2026y / 10 / 20) + 10h + 49min + 44s);

        auto overwrite = sync(session->set_scopes({"onvif://www.onvif.org/location/X"}, options()));
        CHECK(!overwrite && overwrite.error().soap_fault &&
              overwrite.error().soap_fault->subcode == "ter:ScopeOverwrite");
        CHECK(sync(session->remove_scopes({"onvif://www.onvif.org/location/Office%202"}, options())));
        CHECK(sync(session->set_ntp({true, {}, {}}, options())));
    }

    // ---- Device management: recorded camera ----
    {
        auto device = std::make_shared<testing::RecordedCameraTransport>();
        for (const auto operation : {"GetHostname", "GetNetworkInterfaces", "GetDNS",
                                     "GetNetworkDefaultGateway", "GetNetworkProtocols",
                                     "GetDiscoveryMode", "GetUsers"})
            device->serve(operation, std::string(operation) + ".xml");
        device->reply("SetHostname", 200, testing::recorded_envelope("<tds:SetHostnameResponse></tds:SetHostnameResponse>"));
        device->reply("SetDiscoveryMode", 200,
                      testing::recorded_envelope("<tds:SetDiscoveryModeResponse></tds:SetDiscoveryModeResponse>"));
        device->reply("SystemReboot", 200, testing::recorded_envelope(
            "<tds:SystemRebootResponse><tds:Message>Rebooting in 30 seconds</tds:Message></tds:SystemRebootResponse>"));
        auto session = connect(runtime, device);
        auto hostname = sync(session->get_hostname(options()));
        CHECK(hostname && hostname.value().name == "camera" && !hostname.value().from_dhcp);
        auto interfaces = sync(session->get_network_interfaces(options()));
        CHECK(interfaces && interfaces.value().size() == 1);
        if (interfaces && interfaces.value().size() == 1)
        {
            const auto& item = interfaces.value()[0];
            CHECK(item.token == "eth0" && item.name == "eth0" && item.enabled);
            CHECK(item.hardware_address == "00-00-5e-00-53-01" && item.mtu == 1480);
            CHECK(item.ipv4_enabled && item.ipv4_dhcp);
            CHECK(item.ipv4_addresses == std::vector<std::string>{"192.0.2.10/24"});
        }
        auto dns = sync(session->get_dns(options()));
        CHECK(dns && dns.value().from_dhcp && dns.value().dhcp_servers == std::vector<std::string>{"192.0.2.53"});
        CHECK(dns && dns.value().search_domains == std::vector<std::string>{"domain.name"} &&
              dns.value().servers.empty());
        auto gateway = sync(session->get_network_default_gateway(options()));
        CHECK(gateway && gateway.value() == std::vector<std::string>{"192.0.2.1"});
        auto protocols = sync(session->get_network_protocols(options()));
        CHECK(protocols && protocols.value().size() == 2);
        CHECK(protocols && protocols.value()[1].name == "RTSP" && protocols.value()[1].enabled &&
              protocols.value()[1].ports == std::vector<int>{554});
        auto mode = sync(session->get_discovery_mode(options()));
        CHECK(mode && mode.value() == DiscoveryMode::discoverable);
        auto users = sync(session->get_users(options()));
        CHECK(users && users.value().size() == 1 && users.value()[0].username == "admin" &&
              users.value()[0].level == "Administrator");

        CHECK(sync(session->set_hostname("camera-2.lab", options())));
        CHECK(device->last("SetHostname").find("<tds:Name>camera-2.lab</tds:Name>") != std::string::npos);
        for (const auto bad : {"", "two words", "-leading", "trailing-", "a..b", "x<y>"})
        {
            auto refused = sync(session->set_hostname(bad, options()));
            CHECK(!refused && refused.error().category == ErrorCategory::invalid_argument);
        }
        CHECK(sync(session->set_discovery_mode(DiscoveryMode::non_discoverable, options())));
        CHECK(device->last("SetDiscoveryMode").find("<tds:DiscoveryMode>NonDiscoverable</tds:DiscoveryMode>") !=
              std::string::npos);
        auto reboot = sync(session->system_reboot(options()));
        CHECK(reboot && reboot.value() == "Rebooting in 30 seconds");
    }

    // ---- Device management: virtual device ----
    {
        auto device = virtual_device(runtime);
        auto session = connect(runtime, device);
        CHECK(sync(session->set_hostname("renamed", options())));
        auto hostname = sync(session->get_hostname(options()));
        CHECK(hostname && hostname.value().name == "renamed");
        CHECK(sync(session->set_discovery_mode(DiscoveryMode::non_discoverable, options())));
        auto mode = sync(session->get_discovery_mode(options()));
        CHECK(mode && mode.value() == DiscoveryMode::non_discoverable);
        auto interfaces = sync(session->get_network_interfaces(options()));
        CHECK(interfaces && interfaces.value().size() == 1 && !interfaces.value()[0].ipv4_dhcp &&
              interfaces.value()[0].ipv4_addresses == std::vector<std::string>{"192.0.2.10/24"});
        auto dns = sync(session->get_dns(options()));
        CHECK(dns && !dns.value().from_dhcp && dns.value().servers == std::vector<std::string>{"192.0.2.1"});
        CHECK(sync(session->get_network_protocols(options())));
        CHECK(sync(session->get_users(options())));
        CHECK(device->reboot_count() == 0);
        CHECK(sync(session->system_reboot(options())));
        CHECK(device->reboot_count() == 1);
    }

    // ---- Custom requests ----
    {
        auto device = std::make_shared<testing::RecordedCameraTransport>();
        device->serve("GetCapabilities", "GetCapabilities.xml");
        device->serve("GetHostname", "GetHostname.xml");
        device->reply("GetZeroConfiguration", 500, testing::recorded_envelope(
            "<SOAP-ENV:Fault><SOAP-ENV:Code><SOAP-ENV:Value>SOAP-ENV:Receiver</SOAP-ENV:Value>"
            "<SOAP-ENV:Subcode><SOAP-ENV:Value>ter:ActionNotSupported</SOAP-ENV:Value></SOAP-ENV:Subcode>"
            "</SOAP-ENV:Code></SOAP-ENV:Fault>"));
        device->reply("GetAudioOutputs", 200,
                      testing::recorded_envelope("<trt:GetAudioOutputsResponse><trt:AudioOutputs token=\"out\"/>"
                                             "</trt:GetAudioOutputsResponse>"));
        // The recorded capabilities advertise the camera's own host.
        auto connected = sync(DeviceSession::connect(
            "http://192.0.2.10:2020/onvif/device_service", device, make_same_host_policy(), runtime,
            std::nullopt, options()
        ));
        CHECK(connected);
        auto session = connected.value();
        auto hostname = sync(session->call(OnvifService::device, "<tds:GetHostname/>", {}, options()));
        CHECK(hostname && hostname.value().find("<tt:Name>camera</tt:Name>") != std::string::npos);
        // The body is sent as given, not wrapped in another operation element.
        CHECK(device->last("GetHostname").find("<s:Body><tds:GetHostname/></s:Body>") != std::string::npos);
        auto outputs = sync(session->call(OnvifService::media, "<trt:GetAudioOutputs/>",
                                          "GetAudioOutputsResponse", options()));
        CHECK(outputs && outputs.value().find("token=\"out\"") != std::string::npos);
        auto unsupported = sync(session->call(OnvifService::device, "<tds:GetZeroConfiguration/>", {}, options()));
        CHECK(!unsupported && unsupported.error().category == ErrorCategory::soap_fault &&
              unsupported.error().soap_fault->subcode == "ter:ActionNotSupported");
        for (const auto bad : {"", "plain text", "<!-- comment -->", "</tds:GetHostname>"})
        {
            auto refused = sync(session->call(OnvifService::device, bad, {}, options()));
            CHECK(!refused && refused.error().category == ErrorCategory::invalid_argument);
        }
        auto bad_name = sync(session->call(OnvifService::device, "<tds:GetHostname/>", "a b", options()));
        CHECK(!bad_name && bad_name.error().category == ErrorCategory::invalid_argument);
        auto wrong = sync(session->call(OnvifService::device, "<tds:GetHostname/>", "OtherResponse", options()));
        CHECK(!wrong && wrong.error().category == ErrorCategory::protocol_violation);

        auto virtual_session = connect(runtime, virtual_device(runtime));
        auto answered = sync(virtual_session->call(OnvifService::device, "<tds:GetHostname/>", {}, options()));
        CHECK(answered && answered.value().find("virtual-camera") != std::string::npos);
        auto elsewhere = sync(virtual_session->call(OnvifService::media, "<trt:GetAudioOutputs/>", {}, options()));
        CHECK(!elsewhere && elsewhere.error().category == ErrorCategory::unsupported);
    }

    CHECK(bool(runtime->shutdown()));
    if (failures != 0)
    {
        std::cerr << failures << " device config check(s) failed\n";
        return 1;
    }
    std::cout << "device config tests passed\n";
    return 0;
}

/**
 * @file explorer_tests.cpp
 * @brief Explorer API against a virtual device: routing, security, views, and changes.
 */

#include "explorer.hpp"

#include <algorithm>
#include <boost/json.hpp>
#include <chrono>
#include <example_support.hpp>
#include <iostream>
#include <thread>

using namespace wlonvif;
using namespace wlonvif::examples;
namespace json = boost::json;

namespace
{
    int failures = 0;

#define CHECK(expression)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (!(expression))                                                                         \
        {                                                                                          \
            std::cerr << "explorer check failed: " #expression " at line " << __LINE__ << '\n';    \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

    constexpr const char* host = "127.0.0.1:8090";

    struct Client
    {
        explorer::Explorer& application;

        web::Request request(std::string method, std::string target, std::string body = {}) const
        {
            web::Request value;
            value.method = std::move(method);
            value.target = std::move(target);
            value.body = std::move(body);
            value.host = host;
            value.origin = value.method == "GET" ? "" : std::string("http://") + host;
            value.cookie = "wlonvif_explorer=" + application.session_token();
            return value;
        }

        json::object send(const web::Request& value, unsigned expected = 200) const
        {
            const auto response = application.handle(value);
            CHECK(response.status == expected);
            boost::system::error_code error;
            auto parsed = json::parse(response.body, error);
            if (error || !parsed.is_object())
            {
                std::cerr << "not JSON: " << response.body.substr(0, 200) << '\n';
                ++failures;
                return {};
            }
            return parsed.as_object();
        }

        json::object get(std::string target, unsigned expected = 200) const
        {
            return send(request("GET", std::move(target)), expected);
        }

        json::object post(std::string target, std::string body, unsigned expected = 200) const
        {
            return send(request("POST", std::move(target), std::move(body)), expected);
        }
    };

    bool ok(const json::object& reply)
    {
        const auto found = reply.find("ok");
        if (found != reply.end() && found->value().is_bool() && found->value().as_bool())
        {
            return true;
        }
        std::cerr << "API reply: " << json::serialize(reply) << '\n';
        return false;
    }

    const json::value& data(const json::object& reply)
    {
        static const json::value empty;
        const auto found = reply.find("data");
        return found == reply.end() ? empty : found->value();
    }

    /// A section of a view is {"ok":true,"value":...} or {"ok":false,"error":...}.
    bool section_ok(const json::value& value)
    {
        return value.is_object() && value.as_object().contains("ok") && value.at("ok").as_bool();
    }

    std::shared_ptr<VirtualDeviceTransport> virtual_device(const std::shared_ptr<Runtime>& runtime)
    {
        // Everything but Imaging, which the page must then show as missing.
        Capabilities capabilities;
        capabilities.media_xaddr = "http://cam/onvif/media";
        capabilities.ptz_xaddr = "http://cam/onvif/ptz";
        capabilities.events_xaddr = "http://cam/onvif/events";
        capabilities.analytics_xaddr = "http://cam/onvif/analytics";
        auto device = std::make_shared<VirtualDeviceTransport>(
            runtime,
            DeviceInformation{"Acme", "Cam-1", "2.0", "SN1", "HW1"},
            std::vector<std::string>{
                "onvif://www.onvif.org/name/Lobby", "onvif://www.onvif.org/location/Hall"
            },
            std::vector<Service>{
                {"http://www.onvif.org/ver10/device/wsdl", "http://cam/onvif/device_service"},
                {"http://www.onvif.org/ver10/recording/wsdl", "http://cam/onvif/recording"}
            },
            capabilities
        );
        device->add_profile(
            {"main", "Main", "analytics"},
            MediaUri{"rtsp://cam/main", {}, false},
            MediaUri{"", {}, false},
            PtzSpaces{}
        );
        ProfileConfiguration profile;
        profile.token = "main";
        profile.name = "Main";
        profile.ptz_configuration = "ptz";
        profile.video_analytics_configuration = "analytics";
        VideoEncoderConfiguration encoder;
        encoder.token = "encoder";
        encoder.encoding = "H264";
        encoder.resolution = {1920, 1080};
        encoder.quality = 3;
        encoder.frame_rate_limit = 25;
        encoder.encoding_interval = 1;
        encoder.bitrate_limit = 2048;
        profile.video_encoder = encoder;
        device->set_profile_configuration(profile);
        VideoEncoderOptions options;
        options.quality = {1, 5};
        options.encodings.push_back(
            {"H264",
             {{1920, 1080}, {1280, 720}},
             {1, 30},
             {1, 1},
             std::nullopt,
             IntegerRange{64, 4096},
             {"Main"}}
        );
        device->set_video_encoder_options("encoder", options);
        ImagingSettings imaging;
        imaging.brightness = 50;
        ImagingOptions ranges;
        ranges.brightness = ImagingRange{0, 100};
        device->set_video_source({"source", 25, 1920, 1080}, imaging, ranges);
        PtzConfiguration ptz;
        ptz.token = "ptz";
        ptz.direction = {"OFF", "OFF"};
        ptz.eflip_modes = {"OFF", "ON"};
        ptz.reverse_modes = {"OFF", "ON"};
        device->set_ptz_configuration("main", ptz);
        device->set_event_topics(
            {{"tns1:RuleEngine/CellMotionDetector/Motion", true, {"Rule"}, {"IsMotion"}}}
        );
        device->set_analytics(
            "analytics",
            {AnalyticsConfig{"Motion", "tt:CellMotionEngine", {{"Sensitivity", "50"}}, {}}},
            {AnalyticsConfig{"MotionRule", "tt:CellMotionDetector", {{"MinCount", "5"}}, {}}}
        );
        return device;
    }
} // namespace

int main()
{
    auto runtime = Runtime::create().value();
    auto device = virtual_device(runtime);
    std::optional<Credentials> connected_with;
    explorer::ExplorerOptions options;
    options.allowed_hosts = {host};
    options.timeout_seconds = 5;
    auto application = std::make_shared<explorer::Explorer>(
        options,
        runtime,
        [&](const std::string& url,
            const std::optional<Credentials>& credentials) -> Result<std::shared_ptr<DeviceSession>>
        {
            connected_with = credentials;
            if (credentials && credentials->password == "wrong")
            {
                return Error{
                    ErrorCategory::authentication,
                    OperationId::get_services,
                    "authentication failed"
                };
            }
            return wait_for_task(
                DeviceSession::connect(
                    url, device, make_same_host_policy(), runtime, credentials, call_options(5)
                )
            );
        },
        [](NetworkDiscoveryOptions, CallOptions) -> Result<DiscoveryReport>
        {
            DiscoveryReport report;
            DeviceCandidate candidate;
            candidate.address = "192.0.2.7";
            candidate.device_service = "http://192.0.2.7/onvif/device_service";
            candidate.evidence = DeviceEvidence::onvif_response;
            DiscoveryObservation observation;
            observation.scopes = {"onvif://www.onvif.org/name/Scanned"};
            candidate.discovery = observation;
            report.devices.push_back(candidate);
            report.unchecked = {"192.0.2.200"};
            return report;
        },
        [](const std::string& address) -> Result<std::string>
        {
            return "http://" + address + "/onvif/device_service";
        }
    );
    Client client{*application};

    // ---- Pages and request guards ----
    {
        web::Request page = client.request("GET", "/");
        const auto response = application->handle(page);
        CHECK(response.status == 200 && response.content_type.starts_with("text/html"));
        bool cookie = false;
        for (const auto& [name, value] : response.headers)
        {
            cookie = cookie || (name == "Set-Cookie" &&
                                value.find(application->session_token()) != std::string::npos &&
                                value.find("HttpOnly") != std::string::npos &&
                                value.find("SameSite=Strict") != std::string::npos);
        }
        CHECK(cookie);
        CHECK(
            application->content_security_policy().find("script-src 'self'") != std::string::npos
        );

        auto anonymous = client.request("GET", "/api/state");
        anonymous.cookie.clear();
        CHECK(client.send(anonymous, 401).at("error").at("category") == "authentication");
        auto rebound = client.request("GET", "/api/state");
        rebound.host = "evil.example:8090";
        CHECK(client.send(rebound, 403).at("error").at("message") == "unexpected Host header");
        auto forged = client.request("POST", "/api/scan", "{}");
        forged.origin = "http://evil.example";
        CHECK(client.send(forged, 403).at("error").at("message") == "cross-origin request refused");
        CHECK(client.post("/api/scan", "not json", 400).at("ok") == false);
        CHECK(client.get("/api/nowhere", 404).at("ok") == false);
    }

    // ---- Scan merges discovered cameras ----
    {
        CHECK(ok(client.post("/api/scan", R"({"networks":["192.0.2.0/30"]})")));
        json::object scan;
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            scan = data(client.get("/api/scan")).as_object();
            if (!scan.at("running").as_bool())
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(!scan.at("running").as_bool() && scan.at("found").to_number<int>() == 1);
        CHECK(scan.at("unchecked").as_array().size() == 1);
        const auto state = data(client.get("/api/state")).as_object();
        CHECK(state.at("cameras").as_array().size() == 1);
        CHECK(state.at("cameras").as_array()[0].at("name") == "Scanned");
        CHECK(state.at("cameras").as_array()[0].at("evidence") == "confirmed");
    }

    // ---- Add, connect, and fail clearly ----
    std::string id;
    {
        auto added = client.post("/api/cameras", R"({"url":"http://camera/onvif/device_service"})");
        CHECK(ok(added));
        id = std::string(data(added).at("id").as_string());
        const auto base = "/api/cameras/" + id;
        auto refused = client.get(base + "/overview", 409);
        CHECK(refused.at("error").at("category") == "closed-handle");
        auto wrong = client.post(base + "/connect", R"({"username":"admin","password":"wrong"})");
        CHECK(!wrong.at("ok").as_bool());
        CHECK(wrong.at("error").at("category") == "authentication");
        CHECK(wrong.at("error").at("operation") == "GetServices");
        auto connected =
            client.post(base + "/connect", R"({"username":"admin","password":"secret"})");
        CHECK(ok(connected));
        CHECK(data(connected).at("connected").as_bool());
        CHECK(data(connected).at("identity").at("model") == "Cam-1");
        const auto& services = data(connected).at("services");
        CHECK(
            services.at("media").as_bool() && services.at("ptz").as_bool() &&
            services.at("events").as_bool() && services.at("analytics").as_bool() &&
            !services.at("imaging").as_bool()
        );
        // The password is used but never echoed back.
        CHECK(json::serialize(connected).find("secret") == std::string::npos);
        CHECK(connected_with && connected_with->username == "admin");
    }
    const auto base = "/api/cameras/" + id;

    // ---- Views ----
    {
        auto overview = data(client.get(base + "/overview"));
        CHECK(
            section_ok(overview.at("identity")) &&
            overview.at("identity").at("value").at("manufacturer") == "Acme"
        );
        CHECK(section_ok(overview.at("clock")));
        const auto& services = overview.at("services").at("value").as_array();
        CHECK(
            services.size() == 2 &&
            services[1].at("support").as_string().find("not supported") != json::string::npos
        );

        auto media = data(client.get(base + "/media"));
        CHECK(section_ok(media.at("profiles")));
        CHECK(
            media.at("profiles")
                .at("value")
                .as_array()[0]
                .at("video_encoder")
                .at("width")
                .to_number<int>() == 1920
        );
        CHECK(section_ok(media.at("encoder_options").at("encoder")));
        CHECK(media.at("uris").at("main").at("stream").at("value") == "rtsp://cam/main");

        auto changed = client.post(
            base + "/media/encoder",
            R"({"token":"encoder","width":1280,"height":720,"bitrate":4096})"
        );
        CHECK(ok(changed) && data(changed).at("ignored").as_array().empty());
        auto rejected =
            client.post(base + "/media/encoder", R"({"token":"encoder","width":640,"height":480})");
        CHECK(
            !rejected.at("ok").as_bool() &&
            rejected.at("error").at("operation") == "SetVideoEncoderConfiguration"
        );

        auto imaging = data(client.get(base + "/imaging")).as_array();
        CHECK(
            imaging.size() == 1 &&
            imaging[0].at("settings").at("value").at("brightness").to_number<double>() == 50
        );
        CHECK(ok(
            client.post(base + "/imaging", R"({"source":"source","settings":{"brightness":70}})")
        ));
        imaging = data(client.get(base + "/imaging")).as_array();
        CHECK(imaging[0].at("settings").at("value").at("brightness").to_number<double>() == 70);
        auto out_of_range =
            client.post(base + "/imaging", R"({"source":"source","settings":{"brightness":170}})");
        CHECK(!out_of_range.at("ok").as_bool());
        CHECK(!client
                   .post(base + "/imaging", R"({"source":"source","settings":{"brightness":"x"}})")
                   .at("ok")
                   .as_bool());

        auto ptz = data(client.get(base + "/ptz?profile=main"));
        CHECK(
            section_ok(ptz.at("configuration")) &&
            ptz.at("configuration").at("value").at("eflip") == "OFF"
        );
        auto saved = client.post(
            base + "/ptz/preset", R"({"profile":"main","operation":"save","name":"Door"})"
        );
        CHECK(ok(saved));
        const auto preset = std::string(data(saved).as_string());
        auto tour = client.post(
            base + "/ptz/tour",
            R"({"profile":"main","operation":"create","name":"Loop","spots":[{"preset":")" +
                preset + R"(","stay":5}]})"
        );
        CHECK(ok(tour));
        CHECK(ok(client.post(base + "/ptz/direction", R"({"profile":"main","eflip":"ON"})")));
        ptz = data(client.get(base + "/ptz?profile=main"));
        CHECK(ptz.at("presets").at("value").as_array().size() == 1);
        CHECK(ptz.at("tours").at("value").as_array().size() == 1);
        CHECK(ptz.at("configuration").at("value").at("eflip") == "ON");
        // Direct mode: the first intent moves, repeats only renew, a change re-sends, zero stops.
        auto first = client.post(base + "/ptz/intent", R"({"profile":"main","pan":0.5})");
        CHECK(ok(first) && data(first).at("sent") == "move");
        auto repeat = client.post(base + "/ptz/intent", R"({"profile":"main","pan":0.5})");
        CHECK(ok(repeat) && data(repeat).at("sent").is_null());
        auto faster = client.post(base + "/ptz/intent", R"({"profile":"main","pan":0.9})");
        CHECK(ok(faster) && data(faster).at("sent") == "move");
        auto released =
            client.post(base + "/ptz/intent", R"({"profile":"main","pan":0,"tilt":0,"zoom":0})");
        CHECK(ok(released) && data(released).at("sent") == "stop");
        // Without renewals the server stops the move on its own.
        CHECK(ok(client.post(base + "/ptz/intent", R"({"profile":"main","tilt":0.4})")));
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        auto log = data(client.get("/api/log?since=0")).as_array();
        CHECK(
            std::any_of(
                log.begin(),
                log.end(),
                [](const json::value& entry)
                {
                    return entry.at("action").as_string().starts_with("Stop (browser stopped");
                }
            )
        );
        CHECK(
            ok(client.post(base + "/ptz/intent", R"({"profile":"main","pan":0.5,"mode":"smooth"})"))
        );
        CHECK(
            client.post(base + "/ptz/intent", R"({"profile":"main","mode":"sideways"})", 400)
                .at("ok") == false
        );
        CHECK(ok(client.post(base + "/ptz/stop", R"({"profile":"main"})")));
        CHECK(client.post(base + "/ptz/preset", R"({"operation":"goto"})", 400).at("ok") == false);

        auto analytics = data(client.get(base + "/analytics")).as_array();
        CHECK(
            analytics.size() == 1 && analytics[0].at("modules").at("value").as_array().size() == 1
        );
        CHECK(ok(client.post(
            base + "/analytics",
            R"({"configuration":"analytics","kind":"module","name":"Motion","values":{"Sensitivity":"80"}})"
        )));
        analytics = data(client.get(base + "/analytics")).as_array();
        CHECK(
            analytics[0]
                .at("modules")
                .at("value")
                .as_array()[0]
                .at("parameters")
                .as_array()[0]
                .at("value") == "80"
        );
        auto unknown = client.post(
            base + "/analytics",
            R"({"configuration":"analytics","kind":"rule","name":"MotionRule","values":{"Nope":"1"}})"
        );
        CHECK(!unknown.at("ok").as_bool());

        auto device_view = data(client.get(base + "/device"));
        CHECK(section_ok(device_view.at("hostname")) && section_ok(device_view.at("users")));
        CHECK(ok(client.post(base + "/device", R"({"action":"hostname","name":"lab-cam"})")));
        CHECK(
            data(client.get(base + "/device")).at("hostname").at("value").at("name") == "lab-cam"
        );
        CHECK(!client.post(base + "/device", R"({"action":"reboot"})").at("ok").as_bool());
        CHECK(ok(client.post(base + "/device", R"({"action":"reboot","confirm":true})")));
        CHECK(device->reboot_count() == 1);

        auto raw =
            client.post(base + "/call", R"({"service":"device","body":"<tds:GetHostname/>"})");
        CHECK(ok(raw) && std::string(data(raw).as_string()).find("lab-cam") != std::string::npos);
        auto bad = client.post(base + "/call", R"({"service":"device","body":"text"})");
        CHECK(!bad.at("ok").as_bool() && bad.at("error").at("category") == "invalid-argument");

        // Without FFmpeg, video is unavailable but says why.
        auto live = client.post(base + "/live/start", R"({"profile":"main"})");
        CHECK(
            !live.at("ok").as_bool() &&
            live.at("error").at("message").as_string().find("FFmpeg") != json::string::npos
        );
    }

    // ---- Events ----
    {
        auto topics = client.post(base + "/topics", "{}");
        CHECK(ok(topics) && data(topics).as_array().size() == 1);
        CHECK(ok(client.post(
            base + "/events/start", R"({"topics":["tns1:RuleEngine/CellMotionDetector/Motion"]})"
        )));
        Event ignored;
        ignored.topic = "tns1:RuleEngine/TamperDetector/Tamper";
        Event motion;
        motion.topic = "tns1:RuleEngine/CellMotionDetector/Motion";
        motion.data = "IsMotion=true";
        device->emit(ignored);
        device->emit(motion);
        json::array events;
        for (int attempt = 0; attempt < 100 && events.empty(); ++attempt)
        {
            events = data(client.get(base + "/events?since=0")).at("events").as_array();
            if (events.empty())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        CHECK(events.size() == 1 && events[0].at("data") == "IsMotion=true");
        CHECK(ok(client.post(base + "/events/clear", "{}")));
        CHECK(data(client.get(base + "/events?since=0")).at("events").as_array().empty());
        CHECK(ok(client.post(base + "/events/stop", "{}")));
        CHECK(!data(client.get(base + "/events?since=0")).at("running").as_bool());
    }

    // ---- Compatibility check ----
    {
        CHECK(ok(client.post(base + "/check", "{}")));
        json::object report;
        for (int attempt = 0; attempt < 300; ++attempt)
        {
            report = data(client.get(base + "/check")).as_object();
            if (!report.at("running").as_bool())
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(!report.at("running").as_bool());
        const auto& results = report.at("results").as_array();
        int passed = 0;
        bool recording_noted = false;
        for (const auto& result : results)
        {
            const auto status = std::string(result.at("status").as_string());
            passed += status == "pass";
            recording_noted =
                recording_noted ||
                (status == "info" &&
                 result.at("check").as_string().find("recording") != json::string::npos);
            CHECK(
                status == "pass" || status == "info" || status == "unsupported" || status == "fail"
            );
            if (status == "fail")
            {
                std::cerr << "check reported: " << json::serialize(result) << '\n';
            }
        }
        CHECK(passed > 15);
        CHECK(recording_noted);
        CHECK(report.at("library").is_string());
    }

    // ---- Log and forget ----
    {
        auto log = data(client.get("/api/log?since=0")).as_array();
        CHECK(log.size() > 20);
        bool failure_logged = false;
        for (const auto& entry : log)
        {
            failure_logged = failure_logged ||
                             (!entry.at("ok").as_bool() && entry.as_object().contains("error"));
        }
        CHECK(failure_logged);
        const auto last = log.back().at("sequence").to_number<std::uint64_t>();
        CHECK(data(client.get("/api/log?since=" + std::to_string(last))).as_array().empty());
        CHECK(ok(client.post(base + "/forget", "{}")));
        CHECK(client.get(base + "/overview", 404).at("ok") == false);
    }

    application.reset();
    CHECK(bool(runtime->shutdown()));
    if (failures != 0)
    {
        std::cerr << failures << " explorer check(s) failed\n";
        return 1;
    }
    std::cout << "explorer tests passed\n";
    return 0;
}

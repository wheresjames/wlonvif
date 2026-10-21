/**
 * @file demo.cpp
 * @brief The simulated camera behind --demo: profiles, PTZ, imaging, analytics, and events.
 *
 * Built on the library's VirtualDeviceTransport, so every explorer feature can
 * be tried, and the explorer itself tested, without hardware. Its video is an
 * FFmpeg test pattern.
 */

#include "demo.hpp"

namespace wlonvif::examples::explorer
{
    std::shared_ptr<VirtualDeviceTransport>
    make_demo_camera(const std::shared_ptr<Runtime>& runtime)
    {
        Capabilities capabilities;
        capabilities.media_xaddr = demo_url;
        capabilities.ptz_xaddr = demo_url;
        capabilities.events_xaddr = demo_url;
        capabilities.analytics_xaddr = demo_url;
        capabilities.imaging_xaddr = demo_url;
        auto camera = std::make_shared<VirtualDeviceTransport>(
            runtime,
            DeviceInformation{"wlonvif", "Demo Camera", "1.0 (simulated)", "DEMO-0001", "virtual"},
            std::vector<std::string>{
                "onvif://www.onvif.org/name/Demo",
                "onvif://www.onvif.org/hardware/Virtual",
                "onvif://www.onvif.org/location/Nowhere",
                "onvif://www.onvif.org/Profile/Streaming"
            },
            std::vector<Service>{
                {"http://www.onvif.org/ver10/device/wsdl", demo_url},
                {"http://www.onvif.org/ver10/media/wsdl", demo_url},
                {"http://www.onvif.org/ver20/ptz/wsdl", demo_url},
                {"http://www.onvif.org/ver10/events/wsdl", demo_url},
                {"http://www.onvif.org/ver20/imaging/wsdl", demo_url},
                {"http://www.onvif.org/ver20/analytics/wsdl", demo_url},
                {"http://www.onvif.org/ver10/recording/wsdl", demo_url}
            },
            capabilities
        );
        PtzSpaces spaces{{-1, 1}, {-1, 1}, {0, 1}};
        spaces.continuous_pan_tilt.push_back({"", {-1, 1}, {-1, 1}});
        spaces.continuous_zoom.push_back({"", {0, 1}});
        spaces.relative_pan_tilt.push_back({"", {-1, 1}, {-1, 1}});
        spaces.absolute_pan_tilt.push_back({"", {-1, 1}, {-1, 1}});
        for (const auto& [token, name, width, height, bitrate] :
             {std::tuple{"main", "Main stream", 1920, 1080, 4096},
              std::tuple{"sub", "Sub stream", 640, 360, 512}})
        {
            // FFmpeg's generated test pattern stands in for the camera's stream.
            camera->add_profile(
                {token, name, "analytics"},
                MediaUri{
                    "lavfi:testsrc2=size=" + std::to_string(width) + "x" + std::to_string(height) +
                        ":rate=15",
                    {},
                    false
                },
                MediaUri{},
                spaces
            );
            ProfileConfiguration profile;
            profile.token = token;
            profile.name = name;
            profile.fixed = true;
            profile.video_source =
                VideoSourceConfiguration{"source-config", "Source", "camera", 0, 0, 1920, 1080};
            VideoEncoderConfiguration encoder;
            encoder.token = std::string(token) + "-encoder";
            encoder.name = std::string(name) + " encoder";
            encoder.use_count = 1;
            encoder.encoding = "H264";
            encoder.resolution = {width, height};
            encoder.quality = 4;
            encoder.frame_rate_limit = 25;
            encoder.encoding_interval = 1;
            encoder.bitrate_limit = bitrate;
            encoder.gov_length = 50;
            encoder.profile = "Main";
            encoder.multicast_address = "0.0.0.0";
            encoder.session_timeout = "PT60S";
            profile.video_encoder = encoder;
            profile.audio_source = "microphone";
            profile.audio_encoder = AudioEncoderConfiguration{"audio", "Audio", "G711", 64, 8};
            profile.ptz_configuration = "ptz";
            profile.video_analytics_configuration = "analytics";
            camera->set_profile_configuration(profile);
            VideoEncoderOptions options;
            options.quality = {1, 5};
            options.encodings.push_back(
                {"H264",
                 {{1920, 1080}, {1280, 720}, {640, 360}},
                 {1, 30},
                 {1, 1},
                 IntegerRange{10, 100},
                 IntegerRange{256, 8192},
                 {"Baseline", "Main"}}
            );
            camera->set_video_encoder_options(encoder.token, options);
            PtzConfiguration ptz;
            ptz.token = "ptz";
            ptz.name = "PTZ";
            ptz.node_token = "node";
            ptz.default_timeout = std::chrono::seconds(60);
            ptz.direction = {"OFF", "OFF"};
            ptz.eflip_modes = {"OFF", "ON"};
            ptz.reverse_modes = {"OFF", "ON", "AUTO"};
            camera->set_ptz_configuration(token, ptz);
        }
        camera->add_preset("main", {"home", "Home", {0.0, 0.0, 0.0}});
        camera->add_preset("main", {"door", "Front door", {0.5, -0.2, 0.3}});
        ImagingSettings imaging;
        imaging.brightness = 50;
        imaging.contrast = 50;
        imaging.color_saturation = 60;
        imaging.sharpness = 30;
        imaging.ir_cut_filter = "AUTO";
        imaging.white_balance = "AUTO";
        ImagingOptions options;
        options.brightness = options.contrast = options.color_saturation = options.sharpness =
            ImagingRange{0, 100};
        options.ir_cut_filter_modes = {"ON", "OFF", "AUTO"};
        options.white_balance_modes = {"AUTO", "MANUAL"};
        camera->set_video_source({"camera", 25, 1920, 1080}, imaging, options);
        camera->set_event_topics({
            {"tns1:RuleEngine/CellMotionDetector/Motion",
             true,
             {"VideoSourceConfigurationToken", "Rule"},
             {"IsMotion"}},
            {"tns1:RuleEngine/TamperDetector/Tamper",
             true,
             {"VideoSourceConfigurationToken", "Rule"},
             {"IsTamper"}},
        });
        camera->set_analytics(
            "analytics",
            {AnalyticsConfig{
                 "MotionModule",
                 "tt:CellMotionEngine",
                 {{"Sensitivity", "60"}},
                 {{"Layout", "<tt:CellLayout Rows=\"18\" Columns=\"22\"/>"}}
             },
             AnalyticsConfig{"TamperModule", "tt:TamperEngine", {{"Sensitivity", "0"}}, {}}},
            {AnalyticsConfig{
                "MotionRule",
                "tt:CellMotionDetector",
                {{"MinCount", "5"}, {"AlarmOnDelay", "1000"}, {"AlarmOffDelay", "1000"}},
                {}
            }}
        );
        return camera;
    }
} // namespace wlonvif::examples::explorer

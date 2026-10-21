/**
 * @file imaging_media_tests.cpp
 * @brief Imaging settings, video sources, and Media profile configuration.
 */

#include "recorded_camera_transport.hpp"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <optional>

using namespace wlonvif;
using namespace std::chrono_literals;
using testing::RecordedCameraTransport;
using testing::recorded_envelope;

namespace
{
    int failures = 0;

#define CHECK(expression)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (!(expression))                                                                         \
        {                                                                                          \
            std::cerr << "imaging/media check failed: " #expression " at line " << __LINE__      \
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

    std::shared_ptr<DeviceSession> connect(
        const std::shared_ptr<Runtime>& runtime, std::shared_ptr<HttpTransport> transport
    )
    {
        auto session = sync(DeviceSession::connect(
            "http://192.0.2.10:2020/onvif/device_service", std::move(transport),
            make_same_host_policy(), runtime, std::nullopt, options()
        ));
        CHECK(session);
        return session.value();
    }

    std::shared_ptr<RecordedCameraTransport> recorded_camera()
    {
        auto device = std::make_shared<RecordedCameraTransport>();
        device->serve("GetCapabilities", "GetCapabilities.xml");
        device->serve("GetProfiles", "GetProfiles.xml");
        device->serve("GetVideoSources", "GetVideoSources.xml");
        device->serve("GetImagingSettings", "GetImagingSettings.xml");
        device->serve("GetOptions", "GetImagingOptions.xml");
        device->reply("SetImagingSettings", 200,
                      recorded_envelope("<timg:SetImagingSettingsResponse></timg:SetImagingSettingsResponse>"));
        return device;
    }
} // namespace

int main()
{
    auto runtime = Runtime::create().value();

    // ---- Recorded camera: video sources and imaging ----
    {
        auto device = recorded_camera();
        auto session = connect(runtime, device);
        auto sources = sync(session->media().value()->get_video_sources(options()));
        CHECK(sources && sources.value().size() == 1);
        if (sources && sources.value().size() == 1)
        {
            const auto& source = sources.value()[0];
            CHECK(source.token == "raw_vs1" && source.width == 1920 && source.height == 1080);
            CHECK(source.framerate == 25);
        }

        auto imaging = session->imaging().value();
        auto settings = sync(imaging->get_settings("raw_vs1", options()));
        CHECK(settings && settings.value().brightness == 50.0 && settings.value().contrast == 50.0);
        CHECK(settings && settings.value().color_saturation == 50.0 && settings.value().sharpness == 50.0);
        CHECK(settings && !settings.value().ir_cut_filter && !settings.value().white_balance);
        CHECK(device->last("GetImagingSettings").find(
                  "<timg:VideoSourceToken>raw_vs1</timg:VideoSourceToken>") != std::string::npos);

        auto ranges = sync(imaging->get_options("raw_vs1", options()));
        CHECK(ranges && ranges.value().brightness && ranges.value().brightness->maximum == 100);
        CHECK(ranges && ranges.value().sharpness && ranges.value().sharpness->minimum == 0);
        CHECK(ranges && ranges.value().ir_cut_filter_modes.empty());

        ImagingSettings change;
        change.brightness = 55.5;
        change.contrast = 52;
        change.ir_cut_filter = "AUTO";
        change.white_balance = "AUTO";
        CHECK(sync(imaging->set_settings("raw_vs1", change, true, options())));
        const auto body = device->last("SetImagingSettings");
        // Only the given fields are sent, in the schema's element order.
        CHECK(body.find("<timg:ImagingSettings><tt:Brightness>55.5</tt:Brightness>"
                        "<tt:Contrast>52</tt:Contrast><tt:IrCutFilter>AUTO</tt:IrCutFilter>"
                        "<tt:WhiteBalance><tt:Mode>AUTO</tt:Mode></tt:WhiteBalance>"
                        "</timg:ImagingSettings><timg:ForcePersistence>true</timg:ForcePersistence>") !=
              std::string::npos);
        CHECK(body.find("Sharpness") == std::string::npos);
        CHECK(sync(imaging->set_settings("raw_vs1", change, false, options())));
        CHECK(device->last("SetImagingSettings").find("<timg:ForcePersistence>false") != std::string::npos);

        auto empty = sync(imaging->set_settings("raw_vs1", ImagingSettings{}, true, options()));
        CHECK(!empty && empty.error().category == ErrorCategory::invalid_argument);
        ImagingSettings infinite;
        infinite.brightness = std::numeric_limits<double>::infinity();
        auto rejected = sync(imaging->set_settings("raw_vs1", infinite, true, options()));
        CHECK(!rejected && rejected.error().category == ErrorCategory::invalid_argument);
        auto no_source = sync(imaging->set_settings("", change, true, options()));
        CHECK(!no_source && no_source.error().category == ErrorCategory::invalid_argument);
    }

    // ---- Virtual device: imaging round trip ----
    {
        auto device = std::make_shared<VirtualDeviceTransport>(
            runtime, DeviceInformation{}, std::vector<std::string>{}, std::vector<Service>{},
            Capabilities{}
        );
        ImagingSettings initial;
        initial.brightness = 50;
        initial.ir_cut_filter = "AUTO";
        ImagingOptions accepted;
        accepted.brightness = ImagingRange{0, 100};
        accepted.ir_cut_filter_modes = {"ON", "OFF", "AUTO"};
        device->set_video_source({"source", 30, 1280, 720}, initial, accepted);
        auto session = connect(runtime, device);
        auto sources = sync(session->media().value()->get_video_sources(options()));
        CHECK(sources && sources.value().size() == 1 && sources.value()[0].width == 1280);
        auto imaging = session->imaging().value();
        ImagingSettings change;
        change.brightness = 70;
        CHECK(sync(imaging->set_settings("source", change, true, options())));
        auto settings = sync(imaging->get_settings("source", options()));
        CHECK(settings && settings.value().brightness == 70.0 && settings.value().ir_cut_filter == "AUTO");
        change.brightness = 150;
        CHECK(!sync(imaging->set_settings("source", change, true, options())));
        ImagingSettings unsupported;
        unsupported.contrast = 10;
        CHECK(!sync(imaging->set_settings("source", unsupported, true, options())));
        settings = sync(imaging->get_settings("source", options()));
        CHECK(settings && settings.value().brightness == 70.0); // Refusals change nothing.
        CHECK(!sync(imaging->get_settings("missing", options())));
    }

    // ---- Recorded camera: full profiles and encoder options ----
    {
        auto device = recorded_camera();
        device->serve("GetVideoEncoderConfigurationOptions", "GetVideoEncoderConfigurationOptions.xml");
        device->reply("SetVideoEncoderConfiguration", 200, recorded_envelope(
            "<trt:SetVideoEncoderConfigurationResponse></trt:SetVideoEncoderConfigurationResponse>"));
        auto session = connect(runtime, device);
        auto media = session->media().value();
        auto profiles = sync(media->get_profile_configurations(options()));
        CHECK(profiles && profiles.value().size() == 3);
        if (profiles && profiles.value().size() == 3)
        {
            const auto& main = profiles.value()[0];
            CHECK(main.token == "profile_1" && main.name == "mainStream" && main.fixed);
            CHECK(main.video_source && main.video_source->source_token == "raw_vs1" &&
                  main.video_source->width == 1280 && main.video_source->height == 720);
            CHECK(main.video_encoder && main.video_encoder->token == "main" &&
                  main.video_encoder->name == "VideoEncoder_1");
            if (main.video_encoder)
            {
                const auto& encoder = *main.video_encoder;
                CHECK(encoder.encoding == "H264" && encoder.profile == "Main");
                CHECK((encoder.resolution == Resolution{1920, 1080}));
                CHECK(encoder.quality == 5 && encoder.frame_rate_limit == 15 &&
                      encoder.encoding_interval == 1 && encoder.bitrate_limit == 1572);
                CHECK(encoder.gov_length == 25 && encoder.use_count == 1);
                CHECK(encoder.multicast_address == "0.0.0.0" && encoder.session_timeout == "PT0H1M5S");
            }
            CHECK(main.audio_source == "raw_as1");
            CHECK(main.audio_encoder && main.audio_encoder->encoding == "G711" &&
                  main.audio_encoder->sample_rate == 16000);
            CHECK(main.ptz_configuration == "PTZTOKEN");
            CHECK(main.video_analytics_configuration == "VideoAnalyticsToken");
            const auto& jpeg = profiles.value()[2];
            CHECK(jpeg.video_encoder && jpeg.video_encoder->encoding == "JPEG" &&
                  !jpeg.video_encoder->gov_length && jpeg.video_encoder->profile.empty());

            // The whole configuration goes back, with the change applied.
            auto changed = *main.video_encoder;
            changed.resolution = {1280, 720};
            changed.bitrate_limit = 1024;
            CHECK(sync(media->set_video_encoder_configuration(changed, true, options())));
            const auto body = device->last("SetVideoEncoderConfiguration");
            CHECK(body.find("<trt:Configuration token=\"main\"><tt:Name>VideoEncoder_1</tt:Name>"
                            "<tt:UseCount>1</tt:UseCount><tt:Encoding>H264</tt:Encoding><tt:Resolution>"
                            "<tt:Width>1280</tt:Width><tt:Height>720</tt:Height></tt:Resolution>"
                            "<tt:Quality>5</tt:Quality><tt:RateControl><tt:FrameRateLimit>15</tt:FrameRateLimit>"
                            "<tt:EncodingInterval>1</tt:EncodingInterval><tt:BitrateLimit>1024</tt:BitrateLimit>"
                            "</tt:RateControl><tt:H264><tt:GovLength>25</tt:GovLength><tt:H264Profile>Main"
                            "</tt:H264Profile></tt:H264><tt:Multicast><tt:Address><tt:Type>IPv4</tt:Type>"
                            "<tt:IPv4Address>0.0.0.0</tt:IPv4Address></tt:Address><tt:Port>0</tt:Port>"
                            "<tt:TTL>0</tt:TTL><tt:AutoStart>false</tt:AutoStart></tt:Multicast>"
                            "<tt:SessionTimeout>PT0H1M5S</tt:SessionTimeout></trt:Configuration>"
                            "<trt:ForcePersistence>true</trt:ForcePersistence>") != std::string::npos);
            auto jpeg_change = *jpeg.video_encoder;
            CHECK(sync(media->set_video_encoder_configuration(jpeg_change, false, options())));
            CHECK(device->last("SetVideoEncoderConfiguration").find("H264") == std::string::npos);

            auto broken = changed;
            broken.resolution.width = 0;
            auto refused = sync(media->set_video_encoder_configuration(broken, true, options()));
            CHECK(!refused && refused.error().category == ErrorCategory::invalid_argument);
        }

        auto encoder_options = sync(media->get_video_encoder_options("main", options()));
        CHECK(encoder_options && encoder_options.value().quality.minimum == 1 &&
              encoder_options.value().quality.maximum == 5);
        CHECK(encoder_options && encoder_options.value().encodings.size() == 1);
        if (encoder_options && encoder_options.value().encodings.size() == 1)
        {
            const auto& h264 = encoder_options.value().encodings[0];
            CHECK(h264.encoding == "H264" && h264.resolutions.size() == 3);
            CHECK((h264.resolutions[0] == Resolution{1920, 1080}));
            CHECK(h264.frame_rate.minimum == 15 && h264.frame_rate.maximum == 15);
            CHECK(h264.gov_length && h264.gov_length->maximum == 25);
            CHECK(h264.bitrate && h264.bitrate->minimum == 256 && h264.bitrate->maximum == 2048);
            CHECK(h264.profiles == std::vector<std::string>{"Main"});
        }
        device->serve("GetVideoEncoderConfigurationOptions", "GetVideoEncoderConfigurationOptionsJpeg.xml");
        encoder_options = sync(media->get_video_encoder_options("jpeg", options()));
        CHECK(encoder_options && encoder_options.value().encodings.size() == 1 &&
              encoder_options.value().encodings[0].encoding == "JPEG" &&
              !encoder_options.value().encodings[0].gov_length &&
              encoder_options.value().encodings[0].bitrate->maximum == 512);
    }

    // ---- Virtual device: encoder changes honour the options ----
    {
        auto device = std::make_shared<VirtualDeviceTransport>(
            runtime, DeviceInformation{}, std::vector<std::string>{}, std::vector<Service>{},
            Capabilities{}
        );
        VideoEncoderConfiguration encoder;
        encoder.token = "encoder";
        encoder.encoding = "H264";
        encoder.resolution = {1920, 1080};
        encoder.quality = 3;
        encoder.frame_rate_limit = 25;
        encoder.encoding_interval = 1;
        encoder.bitrate_limit = 2048;
        ProfileConfiguration profile;
        profile.token = "main";
        profile.video_encoder = encoder;
        device->set_profile_configuration(profile);
        VideoEncoderOptions accepted;
        accepted.quality = {1, 5};
        accepted.encodings.push_back({"H264", {{1920, 1080}, {1280, 720}}, {1, 30}, {1, 1}, IntegerRange{1, 60},
                                      IntegerRange{64, 4096}, {"Main"}});
        device->set_video_encoder_options("encoder", accepted);
        auto session = connect(runtime, device);
        auto media = session->media().value();
        encoder.resolution = {1280, 720};
        CHECK(sync(media->set_video_encoder_configuration(encoder, true, options())));
        auto profiles = sync(media->get_profile_configurations(options()));
        CHECK(profiles && profiles.value()[0].video_encoder->resolution == (Resolution{1280, 720}));
        encoder.resolution = {640, 480};
        CHECK(!sync(media->set_video_encoder_configuration(encoder, true, options())));
        encoder.resolution = {1280, 720};
        encoder.bitrate_limit = 9000;
        CHECK(!sync(media->set_video_encoder_configuration(encoder, true, options())));
        auto offered = sync(media->get_video_encoder_options("encoder", options()));
        CHECK(offered && offered.value().encodings[0].resolutions.size() == 2);
    }

    CHECK(bool(runtime->shutdown()));
    if (failures != 0)
    {
        std::cerr << failures << " imaging/media check(s) failed\n";
        return 1;
    }
    std::cout << "imaging and media tests passed\n";
    return 0;
}

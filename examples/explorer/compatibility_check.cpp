/**
 * @file compatibility_check.cpp
 * @brief The Compatibility tab: every read-only operation wlonvif supports, run once.
 *
 * The check runs on its own thread and appends one result per operation, so
 * the page can show progress. Each result is `pass` (with a short description
 * of the answer), `unsupported` (the camera says it lacks the feature), `fail`
 * (with the full error), or `info` (a note that needs no request).
 */

#include "implementation.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <wlonvif/version.hpp>

namespace wlonvif::examples::explorer
{
    namespace
    {
        /// "a, b, c" from the names whose flag is set, or @p none when none are.
        std::string
        join_present(std::initializer_list<std::pair<const char*, bool>> items, std::string none)
        {
            std::string text;
            for (const auto& [name, present] : items)
            {
                if (present)
                {
                    text += (text.empty() ? "" : ", ") + std::string(name);
                }
            }
            return text.empty() ? none : text;
        }

        /// One run of the check against one camera.
        class Checker
        {
          public:
            /// @p ffmpeg is the FFmpeg executable, or empty to skip the stream test.
            Checker(
                Explorer::Implementation& explorer,
                std::shared_ptr<Camera> camera,
                std::shared_ptr<DeviceSession> session,
                std::string ffmpeg
            )
                : explorer_(explorer), camera_(std::move(camera)), session_(std::move(session)),
                  ffmpeg_(std::move(ffmpeg))
            {
            }

            void run_all()
            {
                check_device();
                check_media();
                check_imaging();
                check_ptz();
                check_events();
                check_analytics();
            }

          private:
            /// Run one operation and record how it went. @p describe turns the
            /// camera's answer into the short text the report shows.
            template <class T, class Describe>
            bool probe(std::string area, std::string name, Task<Result<T>> task, Describe describe)
            {
                return probe_step(
                    std::move(area),
                    std::move(name),
                    [&]() -> Result<std::string>
                    {
                        auto value = examples::wait_for_task(std::move(task));
                        if (!value)
                        {
                            return value.error();
                        }
                        return describe(value.value());
                    }
                );
            }

            /// Record a step that may take several requests, such as a stream test.
            template <class Step> bool probe_step(std::string area, std::string name, Step step)
            {
                const auto started = SteadyClock::now();
                Result<std::string> result = step();
                const auto elapsed = SteadyClock::now() - started;
                explorer_.record(
                    camera_->id, name, elapsed, Explorer::Implementation::error_of(result)
                );
                const auto milliseconds =
                    std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
                json::object entry{
                    {"area", std::move(area)},
                    {"check", std::move(name)},
                    {"ms", milliseconds.count()}
                };
                if (result)
                {
                    entry["status"] = "pass";
                    entry["detail"] = result.value();
                }
                else
                {
                    entry["status"] = unsupported(result.error()) ? "unsupported" : "fail";
                    entry["error"] = error_json(result.error());
                }
                add(std::move(entry));
                return static_cast<bool>(result);
            }

            /// A note in the report that involved no request.
            void info(std::string area, std::string name, std::string detail)
            {
                add(json::object{
                    {"area", std::move(area)},
                    {"check", std::move(name)},
                    {"status", "info"},
                    {"detail", std::move(detail)},
                    {"ms", 0}
                });
            }

            void add(json::object entry)
            {
                std::lock_guard lock(camera_->check_mutex);
                camera_->check_results.push_back(std::move(entry));
            }

            CallOptions call() const
            {
                return explorer_.call();
            }

            void check_device()
            {
                auto& device = *session_;
                probe(
                    "Device",
                    "GetDeviceInformation",
                    device.get_device_information(call()),
                    [](const DeviceInformation& value)
                    {
                        return value.manufacturer + " " + value.model + ", firmware " +
                               value.firmware_version;
                    }
                );
                probe(
                    "Device",
                    "GetSystemDateAndTime",
                    device.get_system_date_and_time(call()),
                    [](const SystemDateAndTime& value)
                    {
                        const auto skew = std::chrono::round<std::chrono::seconds>(
                            value.utc - std::chrono::system_clock::now()
                        );
                        return std::string(value.type == DateTimeType::ntp ? "NTP" : "manual") +
                               ", clock " + std::to_string(skew.count()) + " s from this computer";
                    }
                );
                probe(
                    "Device",
                    "GetScopes",
                    device.get_scope_entries(call()),
                    [](const std::vector<ScopeEntry>& value)
                    {
                        const auto fixed = std::count_if(
                            value.begin(),
                            value.end(),
                            [](const auto& scope)
                            {
                                return scope.definition == ScopeDefinition::fixed;
                            }
                        );
                        return std::to_string(value.size()) + " scopes, " + std::to_string(fixed) +
                               " fixed";
                    }
                );
                std::vector<Service> services;
                probe(
                    "Device",
                    "GetServices",
                    device.get_services(call()),
                    [&](const std::vector<Service>& value)
                    {
                        services = value;
                        return std::to_string(value.size()) + " services advertised";
                    }
                );
                probe(
                    "Device",
                    "GetCapabilities",
                    device.get_capabilities(call()),
                    [](const Capabilities& value)
                    {
                        return join_present(
                            {{"Media", value.media_xaddr.has_value()},
                             {"PTZ", value.ptz_xaddr.has_value()},
                             {"Events", value.events_xaddr.has_value()},
                             {"Analytics", value.analytics_xaddr.has_value()},
                             {"Imaging", value.imaging_xaddr.has_value()}},
                            "no services"
                        );
                    }
                );
                probe(
                    "Device",
                    "GetNTP",
                    device.get_ntp(call()),
                    [](const NtpConfiguration& value)
                    {
                        return value.from_dhcp
                                   ? std::string("servers from DHCP")
                                   : std::to_string(value.servers.size()) + " manual servers";
                    }
                );
                probe(
                    "Device",
                    "GetHostname",
                    device.get_hostname(call()),
                    [](const HostnameInformation& value)
                    {
                        return value.name;
                    }
                );
                probe(
                    "Device",
                    "GetNetworkInterfaces",
                    device.get_network_interfaces(call()),
                    [](const std::vector<NetworkInterface>& value)
                    {
                        return std::to_string(value.size()) + " interfaces";
                    }
                );
                probe(
                    "Device",
                    "GetDNS",
                    device.get_dns(call()),
                    [](const DnsInformation& value)
                    {
                        return value.from_dhcp ? std::string("from DHCP") : std::string("manual");
                    }
                );
                probe(
                    "Device",
                    "GetNetworkDefaultGateway",
                    device.get_network_default_gateway(call()),
                    [](const std::vector<std::string>& value)
                    {
                        return value.empty() ? std::string("none") : value.front();
                    }
                );
                probe(
                    "Device",
                    "GetNetworkProtocols",
                    device.get_network_protocols(call()),
                    [](const std::vector<NetworkProtocol>& value)
                    {
                        return std::to_string(value.size()) + " protocols";
                    }
                );
                probe(
                    "Device",
                    "GetDiscoveryMode",
                    device.get_discovery_mode(call()),
                    [](DiscoveryMode value)
                    {
                        return value == DiscoveryMode::discoverable ? std::string("discoverable")
                                                                    : std::string("hidden");
                    }
                );
                probe(
                    "Device",
                    "GetUsers",
                    device.get_users(call()),
                    [](const std::vector<DeviceUser>& value)
                    {
                        return std::to_string(value.size()) + " accounts";
                    }
                );

                // Advertised services wlonvif cannot use are worth knowing about.
                for (const auto& service : services)
                {
                    const auto support = service_support(service.namespace_uri);
                    if (support != "supported")
                    {
                        info("Services", service.namespace_uri, "advertised; " + support);
                    }
                }
            }

            void check_media()
            {
                auto media = session_->media();
                if (!media)
                {
                    probe_step(
                        "Media",
                        "Media service",
                        [&]() -> Result<std::string>
                        {
                            return media.error();
                        }
                    );
                    return;
                }
                auto& client = *media.value();
                probe(
                    "Media",
                    "GetProfiles",
                    client.get_profile_configurations(call()),
                    [&](const std::vector<ProfileConfiguration>& value)
                    {
                        profiles_ = value;
                        std::string names;
                        for (const auto& profile : value)
                        {
                            names += (names.empty() ? "" : ", ") + profile.name;
                        }
                        return std::to_string(value.size()) + " profiles: " + names;
                    }
                );
                probe(
                    "Media",
                    "GetVideoSources",
                    client.get_video_sources(call()),
                    [&](const std::vector<VideoSource>& value)
                    {
                        sources_ = value;
                        std::string text;
                        for (const auto& source : value)
                        {
                            text += (text.empty() ? "" : ", ") + source.token + " " +
                                    std::to_string(source.width) + "x" +
                                    std::to_string(source.height);
                        }
                        return text.empty() ? std::string("none") : text;
                    }
                );

                std::set<std::string> encoders; // Profiles often share one encoder.
                for (const auto& profile : profiles_)
                {
                    probe(
                        "Media",
                        "GetStreamUri " + profile.token,
                        client.get_stream_uri(profile.token, call()),
                        [](const MediaUri& value)
                        {
                            return redact_credentials(value.uri);
                        }
                    );
                    probe(
                        "Media",
                        "GetSnapshotUri " + profile.token,
                        client.get_snapshot_uri(profile.token, call()),
                        [](const MediaUri& value)
                        {
                            return value.uri.empty() ? std::string("empty URI")
                                                     : redact_credentials(value.uri);
                        }
                    );
                    if (profile.video_encoder &&
                        encoders.insert(profile.video_encoder->token).second)
                    {
                        probe(
                            "Media",
                            "GetVideoEncoderConfigurationOptions " + profile.video_encoder->token,
                            client.get_video_encoder_options(profile.video_encoder->token, call()),
                            [](const VideoEncoderOptions& value)
                            {
                                std::string text;
                                for (const auto& encoding : value.encodings)
                                {
                                    text += (text.empty() ? "" : ", ") + encoding.encoding + " (" +
                                            std::to_string(encoding.resolutions.size()) + " sizes)";
                                }
                                return text.empty() ? std::string("no encodings") : text;
                            }
                        );
                    }
                }
                if (!ffmpeg_.empty() && !profiles_.empty())
                {
                    probe_step(
                        "Media",
                        "Live stream decodes",
                        [&]
                        {
                            return decode_first_frame(client);
                        }
                    );
                }
            }

            /// Open the stream the live view starts on and wait for one frame.
            Result<std::string> decode_first_frame(MediaClient& client)
            {
                // The smallest H.264/H.265 profile, as in the page: RTSP JPEG streams
                // are often damaged.
                const auto& preview = *std::ranges::min_element(
                    profiles_,
                    {},
                    [](const ProfileConfiguration& profile)
                    {
                        if (!profile.video_encoder)
                        {
                            return std::numeric_limits<long long>::max();
                        }
                        const auto& encoder = *profile.video_encoder;
                        const auto pixels = static_cast<long long>(encoder.resolution.width) *
                                            encoder.resolution.height;
                        return (encoder.encoding == "JPEG" ? 1'000'000'000LL : 0LL) + pixels;
                    }
                );
                auto uri = examples::wait_for_task(client.get_stream_uri(preview.token, call()));
                if (!uri)
                {
                    return uri.error();
                }
                auto video = web::FfmpegVideo::start(
                    examples::url_with_credentials(
                        uri.value().uri, explorer_.credentials_of(*camera_)
                    ),
                    ffmpeg_,
                    2
                );
                if (!video)
                {
                    return video.error();
                }
                std::uint64_t sequence{};
                std::string frame;
                if (video.value()->wait_for_frame(sequence, frame, std::chrono::seconds(12)))
                {
                    return "first frame from " + preview.token + ", " +
                           std::to_string(frame.size()) + " bytes";
                }
                const auto diagnostics = redact_credentials(video.value()->diagnostics());
                return Error{
                    ErrorCategory::network,
                    OperationId::get_stream_uri,
                    "no video frame within 12 s: " + diagnostics
                };
            }

            void check_imaging()
            {
                auto imaging = session_->imaging();
                if (!imaging)
                {
                    return;
                }
                for (const auto& source : sources_)
                {
                    probe(
                        "Imaging",
                        "GetImagingSettings " + source.token,
                        imaging.value()->get_settings(source.token, call()),
                        [](const ImagingSettings& value)
                        {
                            int count = 0;
                            for (const auto& field :
                                 {value.brightness,
                                  value.contrast,
                                  value.color_saturation,
                                  value.sharpness})
                            {
                                count += field.has_value();
                            }
                            return std::to_string(count) + " adjustable values reported";
                        }
                    );
                    probe(
                        "Imaging",
                        "GetOptions " + source.token,
                        imaging.value()->get_options(source.token, call()),
                        [](const ImagingOptions& value)
                        {
                            return value.ir_cut_filter_modes.empty()
                                       ? std::string("ranges reported, no IR cut")
                                       : std::string("ranges and IR cut modes");
                        }
                    );
                }
            }

            void check_ptz()
            {
                const auto with_ptz = std::find_if(
                    profiles_.begin(),
                    profiles_.end(),
                    [](const auto& profile)
                    {
                        return !profile.ptz_configuration.empty();
                    }
                );
                if (with_ptz == profiles_.end())
                {
                    info("PTZ", "PTZ", "no profile has a PTZ configuration");
                    return;
                }
                auto ptz = session_->ptz();
                if (!ptz)
                {
                    return;
                }
                auto& client = *ptz.value();
                const auto token = with_ptz->token;
                probe(
                    "PTZ",
                    "GetConfigurationOptions (spaces)",
                    client.spaces(token, call()),
                    [](const PtzSpaces& value)
                    {
                        return join_present(
                            {{"continuous", !value.continuous_pan_tilt.empty()},
                             {"relative", !value.relative_pan_tilt.empty()},
                             {"absolute", !value.absolute_pan_tilt.empty()},
                             {"zoom", !value.continuous_zoom.empty()}},
                            "no movement spaces"
                        );
                    }
                );
                probe(
                    "PTZ",
                    "GetStatus",
                    client.status(token, call()),
                    [](const PtzStatus& value)
                    {
                        return value.moving ? std::string("moving") : std::string("idle");
                    }
                );
                probe(
                    "PTZ",
                    "GetPresets",
                    client.get_presets(token, call()),
                    [](const std::vector<Preset>& value)
                    {
                        return std::to_string(value.size()) + " presets";
                    }
                );
                probe(
                    "PTZ",
                    "GetPresetTours",
                    client.get_preset_tours(token, call()),
                    [](const std::vector<PresetTour>& value)
                    {
                        return std::to_string(value.size()) + " tours";
                    }
                );
                probe(
                    "PTZ",
                    "GetConfiguration",
                    client.get_configuration(token, call()),
                    [](const PtzConfiguration& value)
                    {
                        return "EFlip " + value.direction.eflip.value_or("n/a") + ", Reverse " +
                               value.direction.reverse.value_or("n/a");
                    }
                );
            }

            void check_events()
            {
                auto events = session_->event_client();
                if (!events)
                {
                    return;
                }
                auto& client = *events.value();
                probe(
                    "Events",
                    "GetEventProperties",
                    client.get_topics(call()),
                    [](const std::vector<EventTopic>& value)
                    {
                        return std::to_string(value.size()) + " topics";
                    }
                );
                // A whole subscription lifecycle, closed again before the check ends.
                probe_step(
                    "Events",
                    "PullPoint subscribe, pull, unsubscribe",
                    [&]() -> Result<std::string>
                    {
                        auto subscription = examples::wait_for_task(
                            client.create_pullpoint({std::chrono::seconds(30), {}}, call())
                        );
                        if (!subscription)
                        {
                            return subscription.error();
                        }
                        auto pulled = examples::wait_for_task(
                            subscription.value()->pull(std::chrono::seconds(1), 10, call())
                        );
                        auto closed =
                            examples::wait_for_task(subscription.value()->unsubscribe(call()));
                        if (!pulled)
                        {
                            return pulled.error();
                        }
                        if (!closed)
                        {
                            return closed.error();
                        }
                        return std::to_string(pulled.value().size()) + " events in 1 s";
                    }
                );
            }

            void check_analytics()
            {
                std::set<std::string> configurations;
                for (const auto& profile : profiles_)
                {
                    if (!profile.video_analytics_configuration.empty())
                    {
                        configurations.insert(profile.video_analytics_configuration);
                    }
                }
                if (configurations.empty())
                {
                    info("Analytics", "Analytics", "no profile has an analytics configuration");
                    return;
                }
                auto analytics = session_->analytics();
                if (!analytics)
                {
                    return;
                }
                for (const auto& configuration : configurations)
                {
                    probe(
                        "Analytics",
                        "GetAnalyticsModules " + configuration,
                        analytics.value()->get_modules(configuration, call()),
                        [](const std::vector<AnalyticsConfig>& value)
                        {
                            std::string names;
                            for (const auto& module : value)
                            {
                                names += (names.empty() ? "" : ", ") + module.name;
                            }
                            return names.empty() ? std::string("no modules") : names;
                        }
                    );
                    probe(
                        "Analytics",
                        "GetRules " + configuration,
                        analytics.value()->get_rules(configuration, call()),
                        [](const std::vector<AnalyticsConfig>& value)
                        {
                            return std::to_string(value.size()) + " rules";
                        }
                    );
                }
            }

            Explorer::Implementation& explorer_;
            std::shared_ptr<Camera> camera_;
            std::shared_ptr<DeviceSession> session_;
            std::string ffmpeg_;
            // Learned by the Media checks and used by the later areas.
            std::vector<ProfileConfiguration> profiles_;
            std::vector<VideoSource> sources_;
        };
    } // namespace

    Reply Explorer::Implementation::start_check(
        const std::shared_ptr<Camera>& camera, const std::shared_ptr<DeviceSession>& session
    )
    {
        std::lock_guard lock(camera->check_mutex);
        if (!camera->check_running)
        {
            if (camera->check_thread.joinable())
            {
                camera->check_thread.join();
            }
            camera->check_running = true;
            camera->check_results.clear();
            camera->check_started = utc_text(std::chrono::system_clock::now());
            camera->check_thread = std::thread(
                [this, camera, session, ffmpeg = owner.options_.ffmpeg]
                {
                    Checker(*this, camera, session, ffmpeg).run_all();
                    std::lock_guard done(camera->check_mutex);
                    camera->check_running = false;
                }
            );
        }
        return success(json::object{{"running", true}});
    }

    Reply Explorer::Implementation::check_status(Camera& camera)
    {
        json::object output;
        {
            std::lock_guard lock(camera.check_mutex);
            output = json::object{
                {"running", camera.check_running},
                {"results", camera.check_results},
                {"started", camera.check_started}
            };
        }
        // The camera and library version make a downloaded report self-contained.
        output["camera"] = summary(camera);
        output["library"] = WLONVIF_VERSION_STRING;
        return success(output);
    }
} // namespace wlonvif::examples::explorer

/**
 * @file media_api.cpp
 * @brief The Media, Imaging, and Analytics tabs.
 *
 * Changes are checked by reading the setting back afterwards, because cameras
 * often acknowledge a change and then keep their own value.
 */

#include "implementation.hpp"

#include <algorithm>

namespace wlonvif::examples::explorer
{
    namespace
    {
        /// A JSON array with each element encoded by to_json().
        template <class T> json::array json_list(const std::vector<T>& values)
        {
            json::array output;
            for (const auto& value : values)
            {
                output.push_back(to_json(value));
            }
            return output;
        }
    } // namespace

    // ---- Media -----------------------------------------------------------------

    Reply Explorer::Implementation::media_view(Camera& camera, DeviceSession& session)
    {
        auto media = session.media();
        if (!media)
        {
            return failure(media.error());
        }
        auto& client = *media.value();
        auto profiles = run(camera.id, "GetProfiles", client.get_profile_configurations(call()));
        auto sources = run(camera.id, "GetVideoSources", client.get_video_sources(call()));

        // Per encoder: what it accepts. Per profile: its stream and snapshot URIs.
        json::object encoders;
        json::object uris;
        if (profiles)
        {
            const auto uri = [](const MediaUri& value)
            {
                return json::value(value.uri);
            };
            for (const auto& profile : profiles.value())
            {
                const auto& encoder = profile.video_encoder;
                if (encoder && !encoders.contains(encoder->token))
                {
                    encoders[encoder->token] = section(
                        run(camera.id,
                            "GetVideoEncoderConfigurationOptions",
                            client.get_video_encoder_options(encoder->token, call()))
                    );
                }
                auto stream =
                    run(camera.id, "GetStreamUri", client.get_stream_uri(profile.token, call()));
                auto snapshot = run(
                    camera.id, "GetSnapshotUri", client.get_snapshot_uri(profile.token, call())
                );
                uris[profile.token] = json::object{
                    {"stream", section(stream, uri)}, {"snapshot", section(snapshot, uri)}
                };
            }
        }

        json::object output;
        output["profiles"] = section(profiles, json_list<ProfileConfiguration>);
        output["sources"] = section(sources, json_list<VideoSource>);
        output["encoder_options"] = encoders;
        output["uris"] = uris;
        output["ffmpeg"] = !owner.options_.ffmpeg.empty();
        output["recordings"] = !owner.options_.recordings.empty();
        return success(output);
    }

    Reply Explorer::Implementation::set_encoder(
        Camera& camera, DeviceSession& session, const json::object& body
    )
    {
        auto media = session.media();
        if (!media)
        {
            return failure(media.error());
        }
        const auto token = text_field(body, "token");
        if (!token)
        {
            return rejection(400, "token is required");
        }

        // Start from the current configuration, so unspecified fields keep their values.
        auto& client = *media.value();
        auto profiles = run(camera.id, "GetProfiles", client.get_profile_configurations(call()));
        if (!profiles)
        {
            return failure(profiles.error());
        }
        std::optional<VideoEncoderConfiguration> encoder;
        for (const auto& profile : profiles.value())
        {
            if (profile.video_encoder && profile.video_encoder->token == *token)
            {
                encoder = profile.video_encoder;
            }
        }
        if (!encoder)
        {
            return failure(invalid("no profile uses encoder " + *token));
        }
        auto requested = *encoder;
        const auto integer = [&](std::string_view key, int& field)
        {
            if (const auto value = number_field(body, key))
            {
                field = static_cast<int>(*value);
            }
        };
        integer("width", requested.resolution.width);
        integer("height", requested.resolution.height);
        integer("fps", requested.frame_rate_limit);
        integer("interval", requested.encoding_interval);
        integer("bitrate", requested.bitrate_limit);
        if (const auto quality = number_field(body, "quality"))
        {
            requested.quality = *quality;
        }
        if (const auto gov = number_field(body, "gov"))
        {
            requested.gov_length = static_cast<int>(*gov);
        }

        auto applied =
            run(camera.id,
                "SetVideoEncoderConfiguration",
                client.set_video_encoder_configuration(requested, true, call()));
        if (!applied)
        {
            return failure(applied.error());
        }

        // Read it back and name every field the camera did not take.
        auto after = run(camera.id, "GetProfiles", client.get_profile_configurations(call()));
        json::array ignored;
        if (after)
        {
            for (const auto& profile : after.value())
            {
                if (!profile.video_encoder || profile.video_encoder->token != *token)
                {
                    continue;
                }
                const auto& stored = *profile.video_encoder;
                const auto check = [&](bool same, const char* name)
                {
                    if (!same)
                    {
                        ignored.push_back(json::string(name));
                    }
                };
                check(stored.resolution == requested.resolution, "resolution");
                check(stored.quality == requested.quality, "quality");
                check(stored.frame_rate_limit == requested.frame_rate_limit, "fps");
                check(stored.encoding_interval == requested.encoding_interval, "interval");
                check(stored.bitrate_limit == requested.bitrate_limit, "bitrate");
                check(stored.gov_length == requested.gov_length, "gov");
                break;
            }
        }
        return success(json::object{{"ignored", ignored}});
    }

    // ---- Imaging ---------------------------------------------------------------

    Reply Explorer::Implementation::imaging_view(Camera& camera, DeviceSession& session)
    {
        auto media = session.media();
        auto imaging = session.imaging();
        if (!media)
        {
            return failure(media.error());
        }
        if (!imaging)
        {
            return failure(imaging.error());
        }
        // Imaging settings belong to video sources, which only Media lists.
        auto sources = run(camera.id, "GetVideoSources", media.value()->get_video_sources(call()));
        if (!sources)
        {
            return failure(sources.error());
        }
        auto& client = *imaging.value();
        json::array output;
        for (const auto& source : sources.value())
        {
            auto settings =
                run(camera.id, "GetImagingSettings", client.get_settings(source.token, call()));
            auto options = run(camera.id, "GetOptions", client.get_options(source.token, call()));
            output.push_back(
                json::object{
                    {"source", to_json(source)},
                    {"settings", section(settings)},
                    {"options", section(options)}
                }
            );
        }
        return success(output);
    }

    Reply Explorer::Implementation::set_imaging(
        Camera& camera, DeviceSession& session, const json::object& body
    )
    {
        auto imaging = session.imaging();
        if (!imaging)
        {
            return failure(imaging.error());
        }
        const auto source = text_field(body, "source");
        const auto settings = body.find("settings");
        if (!source || settings == body.end() || !settings->value().is_object())
        {
            return rejection(400, "source and settings are required");
        }
        auto decoded = imaging_settings_from(settings->value().as_object());
        if (!decoded)
        {
            return failure(decoded.error());
        }
        const bool persist = flag_field(body, "persist").value_or(true);
        auto applied =
            run(camera.id,
                "SetImagingSettings",
                imaging.value()->set_settings(*source, decoded.value(), persist, call()));
        return applied ? success() : failure(applied.error());
    }

    // ---- Analytics -------------------------------------------------------------

    Reply Explorer::Implementation::analytics_view(Camera& camera, DeviceSession& session)
    {
        auto media = session.media();
        auto analytics = session.analytics();
        if (!media)
        {
            return failure(media.error());
        }
        if (!analytics)
        {
            return failure(analytics.error());
        }
        // Analytics configurations are found through the profiles that use them.
        auto profiles =
            run(camera.id, "GetProfiles", media.value()->get_profile_configurations(call()));
        if (!profiles)
        {
            return failure(profiles.error());
        }
        std::map<std::string, json::array> users; // Configuration token -> profile names.
        for (const auto& profile : profiles.value())
        {
            if (!profile.video_analytics_configuration.empty())
            {
                users[profile.video_analytics_configuration].push_back(json::string(profile.name));
            }
        }
        auto& client = *analytics.value();
        json::array output;
        for (auto& [configuration, names] : users)
        {
            auto modules =
                run(camera.id, "GetAnalyticsModules", client.get_modules(configuration, call()));
            auto rules = run(camera.id, "GetRules", client.get_rules(configuration, call()));
            output.push_back(
                json::object{
                    {"configuration", configuration},
                    {"profiles", names},
                    {"modules", section(modules)},
                    {"rules", section(rules)}
                }
            );
        }
        return success(output);
    }

    Reply Explorer::Implementation::set_analytics(
        Camera& camera, DeviceSession& session, const json::object& body
    )
    {
        auto analytics = session.analytics();
        if (!analytics)
        {
            return failure(analytics.error());
        }
        const auto configuration = text_field(body, "configuration");
        const auto kind = text_field(body, "kind"); // "module" or "rule"
        const auto name = text_field(body, "name");
        const auto values = body.find("values");
        if (!configuration || !kind || !name || values == body.end() ||
            !values->value().is_object())
        {
            return rejection(400, "configuration, kind, name, and values are required");
        }

        // Modify the current entry, so its structured items are sent back unchanged.
        auto& client = *analytics.value();
        const bool rule = *kind == "rule";
        auto current =
            rule
                ? run(camera.id, "GetRules", client.get_rules(*configuration, call()))
                : run(camera.id, "GetAnalyticsModules", client.get_modules(*configuration, call()));
        if (!current)
        {
            return failure(current.error());
        }
        auto entry = std::find_if(
            current.value().begin(),
            current.value().end(),
            [&](const auto& item)
            {
                return item.name == *name;
            }
        );
        if (entry == current.value().end())
        {
            return failure(invalid("no " + *kind + " named " + *name));
        }
        for (const auto& [key, value] : values->value().as_object())
        {
            auto item = std::find_if(
                entry->simple_items.begin(),
                entry->simple_items.end(),
                [&](const auto& parameter)
                {
                    return parameter.name == key;
                }
            );
            if (item == entry->simple_items.end())
            {
                return failure(invalid(*name + " has no parameter " + std::string(key)));
            }
            item->value =
                value.is_string() ? std::string(value.as_string()) : json::serialize(value);
        }

        auto applied = rule ? run(camera.id,
                                  "ModifyRules",
                                  client.modify_rules(*configuration, {*entry}, call()))
                            : run(camera.id,
                                  "ModifyAnalyticsModules",
                                  client.modify_modules(*configuration, {*entry}, call()));
        return applied ? success() : failure(applied.error());
    }
} // namespace wlonvif::examples::explorer

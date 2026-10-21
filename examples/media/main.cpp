/**
 * @file main.cpp
 * @brief wlonvif-media: media profiles, stream and snapshot URIs, snapshots, and encoders.
 *
 *     wlonvif-media --ip 192.0.2.10 profiles
 *     wlonvif-media --ip 192.0.2.10 stream-uri --profile main
 *     wlonvif-media --ip 192.0.2.10 snapshot --profile main --output still.jpg
 *     wlonvif-media --ip 192.0.2.10 set-encoder --encoder main --fps 10
 *
 * The library does not decode video: stream-uri and record-command print what
 * to give a player or FFmpeg. snapshot downloads the camera's JPEG itself,
 * checking the snapshot URI against the endpoint policy and capping its size.
 */

#include <algorithm>
#include <charconv>
#include <example_support.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    void usage(const char* executable)
    {
        std::cout << "Usage: " << executable
                  << " (--url DEVICE_URL | --ip ADDRESS) COMMAND [options]\n\n"
                  << "Commands:\n"
                  << "  profiles\n"
                  << "  stream-uri --profile TOKEN\n"
                  << "  record-command --profile TOKEN [--output FILE]\n"
                  << "  snapshot-uri --profile TOKEN\n"
                  << "  snapshot --profile TOKEN --output FILE [--max-bytes COUNT]\n"
                  << "  encoders\n"
                  << "      Show each profile's source, encoders, and what each encoder accepts\n"
                  << "  set-encoder --encoder TOKEN [--resolution WxH] [--quality N] [--fps N]\n"
                  << "              [--interval N] [--bitrate KBPS] [--gov N] [--no-persist]\n"
                  << "      Change a video encoder; values are checked against its options\n\n"
                  << "Common options: --timeout --username --password --password-file"
                     " --password-env --json\n";
    }

    /// Requested changes to a video encoder configuration.
    struct EncoderChange
    {
        std::optional<wlonvif::Resolution> resolution;
        std::optional<double> quality;
        std::optional<int> fps, interval, bitrate, gov;
        bool any() const
        {
            return resolution || quality || fps || interval || bitrate || gov;
        }
    };

    std::string number_text(double value)
    {
        std::ostringstream output;
        output << value;
        return output.str();
    }

    std::string range_text(const wlonvif::IntegerRange& range)
    {
        return range.minimum == range.maximum
                   ? std::to_string(range.minimum)
                   : std::to_string(range.minimum) + "-" + std::to_string(range.maximum);
    }

    std::string resolution_text(const wlonvif::Resolution& resolution)
    {
        return std::to_string(resolution.width) + "x" + std::to_string(resolution.height);
    }

    template <class T> std::optional<T> parse_number(std::string_view text)
    {
        T value{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        {
            return std::nullopt;
        }
        return value;
    }

    void print_profile(const wlonvif::ProfileConfiguration& profile)
    {
        std::cout << profile.token << "  " << profile.name << (profile.fixed ? "  (fixed)" : "")
                  << '\n';
        if (profile.video_source)
        {
            const auto& source = *profile.video_source;
            std::cout << "  Video source    " << source.source_token << "  area " << source.width
                      << 'x' << source.height << '+' << source.x << '+' << source.y << '\n';
        }
        if (profile.video_encoder)
        {
            const auto& encoder = *profile.video_encoder;
            std::cout << "  Video encoder   " << encoder.token << "  " << encoder.encoding;
            if (!encoder.profile.empty())
            {
                std::cout << ' ' << encoder.profile;
            }
            std::cout << "  " << resolution_text(encoder.resolution) << "  "
                      << encoder.frame_rate_limit << " fps  " << encoder.bitrate_limit
                      << " kbit/s  quality " << number_text(encoder.quality);
            if (encoder.gov_length)
            {
                std::cout << "  GOP " << *encoder.gov_length;
            }
            std::cout << '\n';
        }
        if (profile.audio_encoder)
        {
            const auto& audio = *profile.audio_encoder;
            std::cout << "  Audio encoder   " << audio.token << "  " << audio.encoding << "  "
                      << audio.sample_rate << " sample rate  " << audio.bitrate << " bitrate\n";
        }
        if (!profile.ptz_configuration.empty())
        {
            std::cout << "  PTZ             " << profile.ptz_configuration << '\n';
        }
        if (!profile.video_analytics_configuration.empty())
        {
            std::cout << "  Analytics       " << profile.video_analytics_configuration << '\n';
        }
    }

    void print_options(const std::string& token, const wlonvif::VideoEncoderOptions& options)
    {
        std::cout << "Encoder " << token << " accepts  quality " << range_text(options.quality)
                  << '\n';
        for (const auto& encoding : options.encodings)
        {
            std::string sizes;
            for (const auto& size : encoding.resolutions)
            {
                sizes += (sizes.empty() ? "" : ", ") + resolution_text(size);
            }
            std::cout << "  " << encoding.encoding << "  sizes " << sizes << "  fps "
                      << range_text(encoding.frame_rate) << "  interval "
                      << range_text(encoding.encoding_interval);
            if (encoding.bitrate)
            {
                std::cout << "  kbit/s " << range_text(*encoding.bitrate);
            }
            if (encoding.gov_length)
            {
                std::cout << "  GOP " << range_text(*encoding.gov_length);
            }
            for (const auto& profile : encoding.profiles)
            {
                std::cout << "  " << profile;
            }
            std::cout << '\n';
        }
    }

    /// Apply @p change to @p encoder, or explain why @p options forbid it.
    std::optional<std::string> apply_change(
        wlonvif::VideoEncoderConfiguration& encoder,
        const EncoderChange& change,
        const wlonvif::VideoEncoderOptions& options
    )
    {
        const auto encoding = std::find_if(
            options.encodings.begin(),
            options.encodings.end(),
            [&](const auto& item)
            {
                return item.encoding == encoder.encoding;
            }
        );
        if (encoding == options.encodings.end())
        {
            return "the device reports no options for " + encoder.encoding;
        }
        const auto within = [](std::string_view name,
                               double value,
                               const wlonvif::IntegerRange& range) -> std::optional<std::string>
        {
            if (value < range.minimum || value > range.maximum)
            {
                return std::string(name) + " " + number_text(value) + " is outside " +
                       range_text(range);
            }
            return std::nullopt;
        };
        if (change.resolution)
        {
            if (std::find(
                    encoding->resolutions.begin(), encoding->resolutions.end(), *change.resolution
                ) == encoding->resolutions.end())
            {
                return "resolution " + resolution_text(*change.resolution) + " is not offered";
            }
            encoder.resolution = *change.resolution;
        }
        std::optional<std::string> problem;
        if (change.quality && !(problem = within("quality", *change.quality, options.quality)))
        {
            encoder.quality = *change.quality;
        }
        if (!problem && change.fps && !(problem = within("fps", *change.fps, encoding->frame_rate)))
        {
            encoder.frame_rate_limit = *change.fps;
        }
        if (!problem && change.interval &&
            !(problem = within("interval", *change.interval, encoding->encoding_interval)))
        {
            encoder.encoding_interval = *change.interval;
        }
        if (!problem && change.bitrate)
        {
            if (encoding->bitrate)
            {
                problem = within("bitrate", *change.bitrate, *encoding->bitrate);
            }
            if (!problem)
            {
                encoder.bitrate_limit = *change.bitrate;
            }
        }
        if (!problem && change.gov)
        {
            if (!encoding->gov_length)
            {
                problem = "GOP length is not adjustable for " + encoder.encoding;
            }
            else if (!(problem = within("GOP length", *change.gov, *encoding->gov_length)))
            {
                encoder.gov_length = *change.gov;
            }
        }
        return problem;
    }
} // namespace

int main(int argc, char** argv)
{
    auto parsed = wlonvif::examples::parse_common_arguments(argc, argv);
    if (!parsed)
    {
        std::cerr << parsed.error().message << '\n';
        return 2;
    }
    auto common = std::move(parsed.value().common);
    std::string url, address, command, profile, output_path, encoder_token;
    EncoderChange change;
    bool persist = true;
    std::size_t maximum_bytes = 16U * 1024U * 1024U;
    for (std::size_t index = 0; index < parsed.value().remaining.size(); ++index)
    {
        const auto& argument = parsed.value().remaining[index];
        if (argument == "--help" || argument == "-h")
        {
            usage(argv[0]);
            return 0;
        }
        if (argument == "--no-persist")
        {
            persist = false;
            continue;
        }
        if ((argument == "--url" || argument == "--ip" || argument == "--profile" ||
             argument == "--output" || argument == "--max-bytes" || argument == "--encoder" ||
             argument == "--resolution" || argument == "--quality" || argument == "--fps" ||
             argument == "--interval" || argument == "--bitrate" || argument == "--gov") &&
            index + 1 >= parsed.value().remaining.size())
        {
            std::cerr << "Missing value for " << argument << '\n';
            return 2;
        }
        if (argument == "--url")
        {
            url = parsed.value().remaining[++index];
        }
        else if (argument == "--ip")
        {
            address = parsed.value().remaining[++index];
        }
        else if (argument == "--profile")
        {
            profile = parsed.value().remaining[++index];
        }
        else if (argument == "--output")
        {
            output_path = parsed.value().remaining[++index];
        }
        else if (argument == "--encoder")
        {
            encoder_token = parsed.value().remaining[++index];
        }
        else if (argument == "--resolution")
        {
            const std::string_view value(parsed.value().remaining[++index]);
            const auto x = value.find('x');
            const auto width =
                x == value.npos ? std::nullopt : parse_number<int>(value.substr(0, x));
            const auto height =
                x == value.npos ? std::nullopt : parse_number<int>(value.substr(x + 1));
            if (!width || !height)
            {
                std::cerr << "Resolution must look like 1280x720\n";
                return 2;
            }
            change.resolution = wlonvif::Resolution{*width, *height};
        }
        else if (argument == "--quality")
        {
            change.quality = parse_number<double>(parsed.value().remaining[++index]);
            if (!change.quality)
            {
                std::cerr << "Invalid quality\n";
                return 2;
            }
        }
        else if (argument == "--fps" || argument == "--interval" || argument == "--bitrate" ||
                 argument == "--gov")
        {
            auto value = parse_number<int>(parsed.value().remaining[++index]);
            if (!value || *value <= 0)
            {
                std::cerr << "Invalid value for " << argument << '\n';
                return 2;
            }
            (argument == "--fps"        ? change.fps
             : argument == "--interval" ? change.interval
             : argument == "--bitrate"  ? change.bitrate
                                        : change.gov) = value;
        }
        else if (argument == "--max-bytes")
        {
            if (!wlonvif::examples::parse_size(parsed.value().remaining[++index], maximum_bytes))
            {
                std::cerr << "Invalid maximum byte count\n";
                return 2;
            }
        }
        else if (command.empty())
        {
            command = argument;
        }
        else
        {
            std::cerr << "Unknown argument: " << argument << '\n';
            return 2;
        }
    }
    if (url.empty() == address.empty() || command.empty() ||
        (command != "profiles" && command != "encoders" && command != "set-encoder" &&
         profile.empty()) ||
        (command == "set-encoder" && (encoder_token.empty() || !change.any())) ||
        (command == "snapshot" && output_path.empty()))
    {
        usage(argv[0]);
        return 2;
    }
    auto credentials = wlonvif::examples::load_credentials(common);
    if (!credentials)
    {
        std::cerr << credentials.error().message << '\n';
        return 2;
    }
    auto runtime_result = wlonvif::Runtime::create();
    if (!runtime_result)
    {
        std::cerr << runtime_result.error().message << '\n';
        return 1;
    }
    auto runtime = runtime_result.value();
    auto endpoint =
        wlonvif::examples::resolve_device_endpoint(url, address, runtime, common.timeout_seconds);
    if (!endpoint)
    {
        std::cerr << "Endpoint discovery failed: " << endpoint.error().message << '\n';
        (void)runtime->shutdown();
        return 1;
    }
    url = std::move(endpoint.value().url);
    // The response size cap applies to snapshots too, so --max-bytes raises both.
    auto transport = wlonvif::make_curl_transport(runtime, wlonvif::XmlLimits{maximum_bytes});
    auto effective_credentials = credentials.value();
    auto session = wlonvif::examples::connect_device(
        url,
        runtime,
        effective_credentials,
        common.timeout_seconds,
        transport,
        &effective_credentials
    );
    if (!session)
    {
        std::cerr << session.error().message << '\n';
        return 1;
    }
    auto media = session.value()->media();
    if (!media)
    {
        std::cerr << media.error().message << '\n';
        return 1;
    }
    if (command == "profiles")
    {
        auto profiles = wlonvif::examples::wait_for_task(
            media.value()->get_profiles(wlonvif::examples::call_options(common.timeout_seconds))
        );
        if (!profiles)
        {
            std::cerr << profiles.error().message << '\n';
            return 1;
        }
        if (common.json)
        {
            std::cout << '[';
        }
        for (std::size_t index = 0; index < profiles.value().size(); ++index)
        {
            const auto& value = profiles.value()[index];
            if (common.json)
            {
                std::cout << (index ? "," : "") << "{\"token\":\""
                          << wlonvif::examples::json_escape(value.token) << "\",\"name\":\""
                          << wlonvif::examples::json_escape(value.name) << "\"}";
            }
            else
            {
                std::cout << value.token << "  " << value.name << '\n';
            }
        }
        if (common.json)
        {
            std::cout << "]\n";
        }
    }
    else if (command == "encoders" || command == "set-encoder")
    {
        const auto timeout = common.timeout_seconds;
        using wlonvif::examples::call_options;
        using wlonvif::examples::wait_for_task;
        auto profiles =
            wait_for_task(media.value()->get_profile_configurations(call_options(timeout)));
        if (!profiles)
        {
            std::cerr << "Profile query failed: " << profiles.error().message << '\n';
            (void)runtime->shutdown();
            return 1;
        }
        int status = 0;
        if (command == "set-encoder")
        {
            std::optional<wlonvif::VideoEncoderConfiguration> encoder;
            for (const auto& item : profiles.value())
            {
                if (item.video_encoder && item.video_encoder->token == encoder_token)
                {
                    encoder = item.video_encoder;
                }
            }
            auto options = wait_for_task(
                media.value()->get_video_encoder_options(encoder_token, call_options(timeout))
            );
            std::optional<std::string> problem;
            if (!encoder)
            {
                problem = "no profile uses encoder " + encoder_token;
            }
            else if (!options)
            {
                problem = "options query failed: " + options.error().message;
            }
            else
            {
                problem = apply_change(*encoder, change, options.value());
            }
            if (problem)
            {
                std::cerr << "Not applied: " << *problem << '\n';
                (void)runtime->shutdown();
                return 2;
            }
            auto applied = wait_for_task(media.value()->set_video_encoder_configuration(
                *encoder, persist, call_options(timeout)
            ));
            if (!applied)
            {
                std::cerr << "  failed  encoder " << encoder_token << ": "
                          << applied.error().message;
                if (applied.error().soap_fault)
                {
                    std::cerr << " (" << applied.error().soap_fault->subcode << ')';
                }
                std::cerr << '\n';
                status = 1;
            }
            else
            {
                std::cout << "  ok      encoder " << encoder_token << '\n';
                profiles =
                    wait_for_task(media.value()->get_profile_configurations(call_options(timeout)));
                if (!profiles)
                {
                    std::cerr << "Profile query failed: " << profiles.error().message << '\n';
                    (void)runtime->shutdown();
                    return 1;
                }
                // Devices acknowledge the change even when they keep some values
                // fixed, so compare what was asked for with what was stored.
                for (const auto& item : profiles.value())
                {
                    if (!item.video_encoder || item.video_encoder->token != encoder_token)
                    {
                        continue;
                    }
                    const auto& stored = *item.video_encoder;
                    std::vector<std::string> ignored;
                    if (change.resolution && stored.resolution != *change.resolution)
                    {
                        ignored.push_back("resolution");
                    }
                    if (change.quality && stored.quality != *change.quality)
                    {
                        ignored.push_back("quality");
                    }
                    if (change.fps && stored.frame_rate_limit != *change.fps)
                    {
                        ignored.push_back("fps");
                    }
                    if (change.interval && stored.encoding_interval != *change.interval)
                    {
                        ignored.push_back("interval");
                    }
                    if (change.bitrate && stored.bitrate_limit != *change.bitrate)
                    {
                        ignored.push_back("bitrate");
                    }
                    if (change.gov && stored.gov_length != change.gov)
                    {
                        ignored.push_back("GOP length");
                    }
                    std::string list;
                    for (const auto& name : ignored)
                    {
                        list += (list.empty() ? "" : ", ") + name;
                    }
                    if (!list.empty())
                    {
                        std::cerr << "  note    the device kept its own " << list
                                  << "; it may not allow changing them\n";
                    }
                    break;
                }
                std::cout << '\n';
            }
        }
        std::vector<std::string> encoders;
        for (std::size_t index = 0; index < profiles.value().size(); ++index)
        {
            const auto& item = profiles.value()[index];
            if (index)
            {
                std::cout << '\n';
            }
            print_profile(item);
            if (item.video_encoder &&
                std::find(encoders.begin(), encoders.end(), item.video_encoder->token) ==
                    encoders.end())
            {
                encoders.push_back(item.video_encoder->token);
            }
        }
        if (command == "encoders")
        {
            for (const auto& token : encoders)
            {
                auto options = wait_for_task(
                    media.value()->get_video_encoder_options(token, call_options(timeout))
                );
                std::cout << '\n';
                if (options)
                {
                    print_options(token, options.value());
                }
                else
                {
                    std::cout << "Encoder " << token
                              << " options unavailable: " << options.error().message << '\n';
                }
            }
        }
        (void)runtime->shutdown();
        return status;
    }
    else
    {
        const bool stream = command == "stream-uri" || command == "record-command";
        auto uri = stream ? wlonvif::examples::wait_for_task(media.value()->get_stream_uri(
                                profile, wlonvif::examples::call_options(common.timeout_seconds)
                            ))
                          : wlonvif::examples::wait_for_task(media.value()->get_snapshot_uri(
                                profile, wlonvif::examples::call_options(common.timeout_seconds)
                            ));
        if (!uri)
        {
            std::cerr << uri.error().message << '\n';
            return 1;
        }
        // Players and FFmpeg take the credentials in the URL itself.
        const auto playable_uri =
            wlonvif::examples::url_with_credentials(uri.value().uri, effective_credentials);
        if (command == "snapshot")
        {
            if (std::filesystem::exists(output_path))
            {
                std::cerr << "Output file already exists\n";
                return 1;
            }
            auto downloaded = wlonvif::examples::wait_for_task(
                wlonvif::download_binary(
                    uri.value().uri,
                    url,
                    transport,
                    wlonvif::make_same_host_policy(),
                    effective_credentials,
                    maximum_bytes,
                    wlonvif::examples::call_options(common.timeout_seconds)
                )
            );
            if (!downloaded || !downloaded.value().content_type.starts_with("image/"))
            {
                std::cerr << (downloaded ? "Snapshot response is not an image"
                                         : downloaded.error().message)
                          << '\n';
                return 1;
            }
            // Write to a temporary name and rename, so a failed write never leaves
            // a truncated image under the requested name.
            const auto temporary = output_path + ".part";
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                output.write(
                    downloaded.value().bytes.data(),
                    static_cast<std::streamsize>(downloaded.value().bytes.size())
                );
                if (!output)
                {
                    std::filesystem::remove(temporary);
                    std::cerr << "Unable to write snapshot\n";
                    return 1;
                }
            }
            std::filesystem::rename(temporary, output_path);
            std::cout << "Saved " << downloaded.value().bytes.size() << " bytes to " << output_path
                      << '\n';
        }
        else if (common.json)
        {
            std::cout << "{\"uri\":\"" << wlonvif::examples::json_escape(playable_uri) << '"';
            if (command == "record-command")
            {
                const auto destination = output_path.empty() ? "recording.mkv" : output_path;
                const auto record = "ffmpeg -rtsp_transport tcp -i " +
                                    wlonvif::examples::shell_quote(playable_uri) +
                                    " -map 0 -c copy " +
                                    wlonvif::examples::shell_quote(destination);
                std::cout << ",\"command\":\"" << wlonvif::examples::json_escape(record) << '"';
            }
            std::cout << "}\n";
        }
        else
        {
            std::cout << playable_uri << '\n';
            if (command == "record-command")
            {
                const auto destination = output_path.empty() ? "recording.mkv" : output_path;
                std::cout << "ffmpeg -rtsp_transport tcp -i "
                          << wlonvif::examples::shell_quote(playable_uri) << " -map 0 -c copy "
                          << wlonvif::examples::shell_quote(destination) << '\n';
            }
            else if (stream)
            {
                std::cout << "VLC: vlc " << wlonvif::examples::shell_quote(playable_uri) << '\n'
                          << "mpv: mpv " << wlonvif::examples::shell_quote(playable_uri) << '\n'
                          << "FFmpeg: ffmpeg -rtsp_transport tcp -i "
                          << wlonvif::examples::shell_quote(playable_uri)
                          << " -c copy recording.mkv\n";
            }
        }
    }
    (void)runtime->shutdown();
    return 0;
}

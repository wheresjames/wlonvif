/** @file main.cpp @brief Inspect real ONVIF Media profiles and retrieve snapshots. */

#include <example_support.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace
{
    void usage(const char* executable)
    {
        std::cout
            << "Usage: " << executable << " (--url DEVICE_URL | --ip ADDRESS) COMMAND [options]\n\n"
            << "Commands:\n"
            << "  profiles\n"
            << "  stream-uri --profile TOKEN\n"
            << "  record-command --profile TOKEN [--output FILE]\n"
            << "  snapshot-uri --profile TOKEN\n"
            << "  snapshot --profile TOKEN --output FILE [--max-bytes COUNT]\n\n"
            << "Common options: --timeout --username --password --password-file"
               " --password-env --json\n";
    }

}

int main(int argc, char** argv)
{
    auto parsed = wlonvif::examples::parse_common_arguments(argc, argv);
    if (!parsed)
    {
        std::cerr << parsed.error().message << '\n';
        return 2;
    }
    auto common = std::move(parsed.value().common);
    std::string url, address, command, profile, output_path;
    std::size_t maximum_bytes = 16U * 1024U * 1024U;
    for (std::size_t index = 0; index < parsed.value().remaining.size(); ++index)
    {
        const auto& argument = parsed.value().remaining[index];
        if (argument == "--help" || argument == "-h")
        {
            usage(argv[0]);
            return 0;
        }
        if ((argument == "--url" || argument == "--ip" || argument == "--profile" || argument == "--output" ||
             argument == "--max-bytes") &&
            index + 1 >= parsed.value().remaining.size())
        {
            std::cerr << "Missing value for " << argument << '\n';
            return 2;
        }
        if (argument == "--url") url = parsed.value().remaining[++index];
        else if (argument == "--ip") address = parsed.value().remaining[++index];
        else if (argument == "--profile") profile = parsed.value().remaining[++index];
        else if (argument == "--output") output_path = parsed.value().remaining[++index];
        else if (argument == "--max-bytes")
        {
            if (!wlonvif::examples::parse_size(
                    parsed.value().remaining[++index], maximum_bytes
                ))
            {
                std::cerr << "Invalid maximum byte count\n";
                return 2;
            }
        }
        else if (command.empty()) command = argument;
        else
        {
            std::cerr << "Unknown argument: " << argument << '\n';
            return 2;
        }
    }
    if (url.empty() == address.empty() || command.empty() ||
        (command != "profiles" && profile.empty()) ||
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
    auto endpoint = wlonvif::examples::resolve_device_endpoint(
        url, address, runtime, common.timeout_seconds
    );
    if (!endpoint)
    {
        std::cerr << "Endpoint discovery failed: " << endpoint.error().message << '\n';
        (void)runtime->shutdown();
        return 1;
    }
    url = std::move(endpoint.value().url);
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
        if (common.json) std::cout << '[';
        for (std::size_t index = 0; index < profiles.value().size(); ++index)
        {
            const auto& value = profiles.value()[index];
            if (common.json)
                std::cout << (index ? "," : "") << "{\"token\":\""
                          << wlonvif::examples::json_escape(value.token) << "\",\"name\":\""
                          << wlonvif::examples::json_escape(value.name) << "\"}";
            else std::cout << value.token << "  " << value.name << '\n';
        }
        if (common.json) std::cout << "]\n";
    }
    else
    {
        const bool stream = command == "stream-uri" || command == "record-command";
        auto uri = stream
                       ? wlonvif::examples::wait_for_task(media.value()->get_stream_uri(
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
        const auto playable_uri =
            wlonvif::examples::url_with_credentials(uri.value().uri, effective_credentials);
        if (command == "snapshot")
        {
            if (std::filesystem::exists(output_path))
            {
                std::cerr << "Output file already exists\n";
                return 1;
            }
            auto downloaded = wlonvif::examples::wait_for_task(wlonvif::download_binary(
                uri.value().uri,
                url,
                transport,
                wlonvif::make_same_host_policy(),
                effective_credentials,
                maximum_bytes,
                wlonvif::examples::call_options(common.timeout_seconds)
            ));
            if (!downloaded || !downloaded.value().content_type.starts_with("image/"))
            {
                std::cerr << (downloaded ? "Snapshot response is not an image"
                                         : downloaded.error().message)
                          << '\n';
                return 1;
            }
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
            std::cout << "Saved " << downloaded.value().bytes.size() << " bytes to "
                      << output_path << '\n';
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
                std::cout << ",\"command\":\""
                          << wlonvif::examples::json_escape(record) << '"';
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

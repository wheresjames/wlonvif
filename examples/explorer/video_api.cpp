/**
 * @file video_api.cpp
 * @brief The Live & recording tab: MJPEG live video, snapshots, and recordings.
 *
 * Browsers cannot play RTSP, so FFmpeg turns the camera's stream into JPEG
 * frames that are sent as an MJPEG response. One FFmpeg process per camera is
 * shared by every viewer, and stops when the last viewer leaves. Recordings
 * use a separate FFmpeg process that copies the stream into a file unchanged.
 */

#include "implementation.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace wlonvif::examples::explorer
{
    Result<std::string> Explorer::Implementation::stream_input(
        Camera& camera, DeviceSession& session, const std::string& profile
    )
    {
        auto media = session.media();
        if (!media)
        {
            return media.error();
        }
        auto uri = run(camera.id, "GetStreamUri", media.value()->get_stream_uri(profile, call()));
        if (!uri)
        {
            return uri.error();
        }
        return examples::url_with_credentials(uri.value().uri, credentials_of(camera));
    }

    // ---- Live video ------------------------------------------------------------

    Reply Explorer::Implementation::start_video(
        Camera& camera, DeviceSession& session, const std::string& profile
    )
    {
        if (owner.options_.ffmpeg.empty())
        {
            return failure(
                Error{
                    ErrorCategory::unsupported,
                    OperationId::get_stream_uri,
                    "live video needs FFmpeg; start the explorer with --ffmpeg PATH"
                }
            );
        }
        {
            // Already running for this profile: share it.
            std::lock_guard lock(camera.video_mutex);
            if (camera.video && camera.video_profile == profile && !camera.video->finished())
            {
                return success();
            }
        }
        auto input = stream_input(camera, session, profile);
        if (!input)
        {
            return failure(input.error());
        }
        auto video = timed(
            camera.id,
            "Start FFmpeg",
            [&]
            {
                return web::FfmpegVideo::start(input.value(), owner.options_.ffmpeg, 10);
            }
        );
        if (!video)
        {
            return failure(video.error());
        }
        std::lock_guard lock(camera.video_mutex);
        camera.video = video.value();
        camera.video_profile = profile;
        return success();
    }

    void Explorer::Implementation::stop_video(Camera& camera)
    {
        // Destroying the source stops FFmpeg; do that outside the lock.
        std::shared_ptr<web::FfmpegVideo> video;
        {
            std::lock_guard lock(camera.video_mutex);
            video = std::exchange(camera.video, nullptr);
        }
    }

    Reply Explorer::Implementation::video_status(Camera& camera)
    {
        std::lock_guard lock(camera.video_mutex);
        if (!camera.video)
        {
            return success(json::object{{"running", false}});
        }
        return success(
            json::object{
                {"running", !camera.video->finished()},
                {"profile", camera.video_profile},
                {"viewers", camera.viewers},
                // FFmpeg echoes the input URL, which carries the password.
                {"diagnostics", redact_credentials(camera.video->diagnostics())}
            }
        );
    }

    void Explorer::Implementation::stream_video(Camera& camera, web::ResponseStream& output)
    {
        std::shared_ptr<web::FfmpegVideo> video;
        {
            std::lock_guard lock(camera.video_mutex);
            video = camera.video;
            if (video)
            {
                ++camera.viewers;
            }
        }
        if (!video)
        {
            return;
        }
        web::serve_mjpeg(
            *video,
            output,
            [&video]
            {
                return video->finished();
            }
        );
        std::lock_guard lock(camera.video_mutex);
        // The last viewer leaving stops FFmpeg, unless it was replaced meanwhile.
        if (--camera.viewers == 0 && camera.video == video)
        {
            camera.video.reset();
        }
    }

    // ---- Snapshots -------------------------------------------------------------

    web::Response Explorer::Implementation::snapshot(
        Camera& camera, DeviceSession& session, const std::string& profile
    )
    {
        const auto image = [](std::string bytes, std::string source)
        {
            web::Response response;
            response.content_type = "image/jpeg";
            response.body = std::move(bytes);
            // Tells the page which of the two ways below produced the image.
            response.headers.emplace_back("X-Snapshot-Source", std::move(source));
            return response;
        };
        json::array attempts; // Why each way failed, if none works.

        // First the ONVIF snapshot URI: it is what the device offers for stills.
        if (auto media = session.media())
        {
            auto uri =
                run(camera.id, "GetSnapshotUri", media.value()->get_snapshot_uri(profile, call()));
            if (uri && !uri.value().uri.empty())
            {
                constexpr std::size_t largest_image = 16U * 1024U * 1024U;
                auto downloaded =
                    run(camera.id,
                        "Snapshot download",
                        download_binary(
                            uri.value().uri,
                            camera.device_url,
                            make_curl_transport(owner.runtime_),
                            make_same_host_policy(),
                            credentials_of(camera),
                            largest_image,
                            call()
                        ));
                if (downloaded && downloaded.value().content_type.starts_with("image/"))
                {
                    return image(std::move(downloaded.value().bytes), "onvif");
                }
                if (!downloaded)
                {
                    attempts.push_back(error_json(downloaded.error()));
                }
                else
                {
                    const auto type = downloaded.value().content_type;
                    attempts.push_back(json::object{{"message", "snapshot response is " + type}});
                }
            }
            else if (!uri)
            {
                attempts.push_back(error_json(uri.error()));
            }
        }

        // Otherwise a frame from the stream: the running live view, or a short FFmpeg run.
        std::shared_ptr<web::FfmpegVideo> video;
        {
            std::lock_guard lock(camera.video_mutex);
            if (camera.video && camera.video_profile == profile)
            {
                video = camera.video;
            }
        }
        if (!video && !owner.options_.ffmpeg.empty())
        {
            auto input = stream_input(camera, session, profile);
            if (!input)
            {
                attempts.push_back(error_json(input.error()));
            }
            else if (auto started =
                         web::FfmpegVideo::start(input.value(), owner.options_.ffmpeg, 2))
            {
                video = started.value();
            }
            else
            {
                attempts.push_back(error_json(started.error()));
            }
        }
        if (video)
        {
            std::uint64_t sequence{};
            std::string frame;
            if (video->wait_for_frame(sequence, frame, std::chrono::seconds(12)))
            {
                return image(std::move(frame), "stream");
            }
            const auto diagnostics = redact_credentials(video->diagnostics());
            attempts.push_back(
                json::object{{"message", "no frame from the stream: " + diagnostics}}
            );
        }

        web::Response response;
        response.body = json::serialize(
            json::object{
                {"ok", false},
                {"error",
                 json::object{
                     {"category", "unsupported"},
                     {"operation", "Snapshot"},
                     {"message", "no snapshot source worked"},
                     {"attempts", attempts}
                 }}
            }
        );
        return response;
    }

    // ---- Recordings ------------------------------------------------------------

    Reply Explorer::Implementation::start_recording(
        Camera& camera, DeviceSession& session, const json::object& body
    )
    {
        if (owner.options_.ffmpeg.empty() || owner.options_.recordings.empty())
        {
            return failure(
                Error{
                    ErrorCategory::unsupported,
                    OperationId::get_stream_uri,
                    "recording needs FFmpeg and --recordings DIR"
                }
            );
        }
        const auto profile = text_field(body, "profile").value_or("");
        const auto seconds = std::clamp(number_field(body, "seconds").value_or(30), 1.0, 3600.0);
        auto input = stream_input(camera, session, profile);
        if (!input)
        {
            return failure(input.error());
        }

        // Name the file after the camera, profile, and start time:
        // 192_168_1_20-main-20261020T101500.mkv
        std::error_code error;
        std::filesystem::create_directories(owner.options_.recordings, error);
        std::string stamp;
        for (const char c : utc_text(std::chrono::system_clock::now()))
        {
            if (std::isdigit(static_cast<unsigned char>(c)) || c == 'T')
            {
                stamp += c;
            }
        }
        const auto name =
            safe_file_part(camera.address) + "-" + safe_file_part(profile) + "-" + stamp + ".mkv";

        auto recording = timed(
            camera.id,
            "Start recording",
            [&]
            {
                return Recording::start(
                    owner.options_.ffmpeg,
                    input.value(),
                    owner.options_.recordings / name,
                    std::chrono::seconds(static_cast<long long>(seconds))
                );
            }
        );
        if (!recording)
        {
            return failure(recording.error());
        }
        std::lock_guard lock(recording_mutex);
        recordings[name] = recording.value();
        return success(json::value(name));
    }

    Reply Explorer::Implementation::recordings_view()
    {
        if (owner.options_.recordings.empty())
        {
            return success(json::array{});
        }
        // The directory is the source of truth, so files from earlier runs are listed too.
        std::error_code error;
        std::vector<std::filesystem::directory_entry> files;
        for (const auto& entry :
             std::filesystem::directory_iterator(owner.options_.recordings, error))
        {
            if (entry.is_regular_file() && recording_name_valid(entry.path().filename().string()))
            {
                files.push_back(entry);
            }
        }
        // Newest first; the names start with the same prefix and end with a timestamp.
        std::sort(
            files.begin(),
            files.end(),
            [](const auto& left, const auto& right)
            {
                return left.path().filename() > right.path().filename();
            }
        );

        json::array output;
        std::lock_guard lock(recording_mutex);
        for (const auto& file : files)
        {
            const auto name = file.path().filename().string();
            json::object item{
                {"name", name},
                {"bytes", static_cast<std::uint64_t>(file.file_size(error))},
                {"active", false}
            };
            if (const auto found = recordings.find(name); found != recordings.end())
            {
                auto& recording = *found->second;
                item["active"] = recording.active();
                item["seconds"] = recording.duration().count();
                if (!recording.active() && recording.exit_status() != 0)
                {
                    item["log"] = read_tail(file.path().string() + ".log", 1500);
                }
            }
            output.push_back(std::move(item));
        }
        return success(output);
    }

    Reply
    Explorer::Implementation::recording_action(const std::string& name, const std::string& action)
    {
        if (!recording_name_valid(name))
        {
            return rejection(400, "invalid recording name");
        }
        std::shared_ptr<Recording> recording;
        {
            std::lock_guard lock(recording_mutex);
            if (const auto found = recordings.find(name); found != recordings.end())
            {
                recording = found->second;
            }
        }
        if (action == "stop")
        {
            if (recording)
            {
                recording->stop();
            }
            return success();
        }
        if (action == "delete")
        {
            if (recording)
            {
                recording->stop();
            }
            std::error_code error;
            std::filesystem::remove(owner.options_.recordings / name, error);
            std::filesystem::remove(owner.options_.recordings / (name + ".log"), error);
            std::lock_guard lock(recording_mutex);
            recordings.erase(name);
            return success();
        }
        return rejection(404, "unknown recording action");
    }

    void Explorer::Implementation::download(const std::string& name, web::ResponseStream& output)
    {
        if (owner.options_.recordings.empty() || !recording_name_valid(name))
        {
            return;
        }
        std::ifstream file(owner.options_.recordings / name, std::ios::binary);
        if (!file)
        {
            return;
        }
        const std::vector<std::pair<std::string, std::string>> headers{
            {"Content-Disposition", "attachment; filename=\"" + name + "\""}
        };
        if (!output.begin(200, "video/x-matroska", headers))
        {
            return;
        }
        // Streamed in pieces: recordings can be far larger than memory should hold.
        std::string buffer(64 * 1024, '\0');
        while (file && !output.stopping())
        {
            file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = static_cast<std::size_t>(file.gcount());
            if (count == 0 || !output.write(std::string_view(buffer.data(), count)))
            {
                break;
            }
        }
    }
} // namespace wlonvif::examples::explorer

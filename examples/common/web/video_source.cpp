/**
 * @file video_source.cpp
 * @brief Run FFmpeg as a child process and serve its JPEG frames as MJPEG.
 *
 * FFmpeg decodes the camera stream and writes JPEG images back to back on
 * stdout; a reader thread splits them at the JPEG start and end markers and
 * keeps only the newest. FFmpeg's stderr is kept (the last 4 kB) so a failed
 * stream can be explained. The options favour low delay over smoothness.
 */

#include "video_source.hpp"

#include "web_server.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <optional>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace wlonvif::examples::web
{
    namespace
    {
        Error launch_error(std::string message)
        {
            return Error{ErrorCategory::network, OperationId::get_stream_uri, std::move(message)};
        }
    } // namespace

    Result<std::shared_ptr<FfmpegVideo>>
    FfmpegVideo::start(std::string input_url, std::string executable, int frames_per_second)
    {
        if (input_url.empty())
        {
            return launch_error("FFmpeg input URL is empty");
        }
        auto result = std::shared_ptr<FfmpegVideo>(new FfmpegVideo);
#ifdef _WIN32
        (void)executable;
        return launch_error("the simple FFmpeg bridge is not yet available on Windows");
#else
        int pipe_descriptors[2]{};
        if (pipe(pipe_descriptors) != 0)
        {
            return launch_error("unable to create FFmpeg output pipe");
        }
        int error_descriptors[2]{};
        if (pipe(error_descriptors) != 0)
        {
            close(pipe_descriptors[0]);
            close(pipe_descriptors[1]);
            return launch_error("unable to create FFmpeg diagnostic pipe");
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, pipe_descriptors[1], STDOUT_FILENO);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_adddup2(&actions, error_descriptors[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipe_descriptors[0]);
        posix_spawn_file_actions_addclose(&actions, pipe_descriptors[1]);
        posix_spawn_file_actions_addclose(&actions, error_descriptors[0]);
        posix_spawn_file_actions_addclose(&actions, error_descriptors[1]);
        // Live viewing favours delay over smoothness: decode each packet as it
        // arrives instead of queueing input, and cap stream probing at one
        // second rather than five for cameras whose SDP omits codec details.
        std::vector<std::string> arguments{
            executable,
            "-nostdin",
            "-hide_banner",
            "-loglevel",
            "error",
            "-fflags",
            "nobuffer",
            "-flags",
            "low_delay",
            "-analyzeduration",
            "1000000"
        };
        // Some cameras send timestamps that step backwards; arrival time is
        // monotonic and is all a live view needs.
        if (!input_url.starts_with("lavfi:"))
        {
            arguments.insert(arguments.end(), {"-use_wallclock_as_timestamps", "1"});
        }
        const bool rtsp = input_url.starts_with("rtsp://") || input_url.starts_with("rtsps://");
        if (rtsp)
        {
            arguments.insert(arguments.end(), {"-rtsp_transport", "tcp"});
        }
        else if (input_url.starts_with("lavfi:"))
        {
            // A generated test source, such as lavfi:testsrc2=size=1280x720:rate=10.
            arguments.insert(arguments.end(), {"-re", "-f", "lavfi"});
            input_url.erase(0, 6);
        }
        else if (!input_url.starts_with("http://") && !input_url.starts_with("https://"))
        {
            arguments.push_back("-re");
        }
        // Every frame is passed through: an fps filter would hold each frame
        // until the next one arrives. read_frames() thins them instead.
        arguments.insert(
            arguments.end(),
            {"-i",
             std::move(input_url),
             "-map",
             "0:v:0",
             "-an",
             "-fps_mode",
             "passthrough",
             "-q:v",
             "5",
             "-f",
             "image2pipe",
             "-vcodec",
             "mjpeg",
             "pipe:1"}
        );
        result->interval_ = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / (frames_per_second > 0 ? frames_per_second : 15))
        );
        std::vector<char*> argv;
        for (auto& argument : arguments)
        {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        const int status = posix_spawnp(
            &result->process_, executable.c_str(), &actions, nullptr, argv.data(), environ
        );
        posix_spawn_file_actions_destroy(&actions);
        close(pipe_descriptors[1]);
        close(error_descriptors[1]);
        if (status != 0)
        {
            close(pipe_descriptors[0]);
            close(error_descriptors[0]);
            return launch_error("unable to launch FFmpeg: " + std::string(std::strerror(status)));
        }
        result->output_ = pipe_descriptors[0];
        result->errors_ = error_descriptors[0];
        result->reader_ = std::thread(
            [pointer = result.get()]
            {
                pointer->read_frames();
            }
        );
        result->diagnostic_reader_ = std::thread(
            [pointer = result.get()]
            {
                pointer->read_diagnostics();
            }
        );
        return result;
#endif
    }

    void FfmpegVideo::read_frames()
    {
#ifndef _WIN32
        constexpr std::size_t maximum_frame = 4 * 1024 * 1024;
        std::string pending;
        std::array<char, 65536> bytes{};
        // Frames arrive in bursts, so the rate limit is a budget rather than a
        // minimum gap: a frame may come a quarter interval early, and a late
        // frame lets the next one through at once, but credit never builds up
        // beyond that.
        std::optional<std::chrono::steady_clock::time_point> due;
        while (true)
        {
            const auto count = read(output_, bytes.data(), bytes.size());
            if (count <= 0)
            {
                break;
            }
            pending.append(bytes.data(), static_cast<std::size_t>(count));
            while (true)
            {
                const auto start = pending.find("\xff\xd8");
                if (start == std::string::npos)
                {
                    pending.clear();
                    break;
                }
                const auto end = pending.find("\xff\xd9", start + 2);
                if (end == std::string::npos)
                {
                    if (start)
                    {
                        pending.erase(0, start);
                    }
                    if (pending.size() > maximum_frame)
                    {
                        pending.clear();
                    }
                    break;
                }
                const auto length = end + 2 - start;
                const auto now = std::chrono::steady_clock::now();
                if (length <= maximum_frame && (!due || now + interval_ / 4 >= *due))
                {
                    due = std::max(due.value_or(now) + interval_, now);
                    std::lock_guard lock(mutex_);
                    frame_.assign(pending, start, length);
                    ++sequence_;
                    changed_.notify_all();
                }
                pending.erase(0, end + 2);
            }
        }
        {
            std::lock_guard lock(mutex_);
            finished_ = true;
        }
        changed_.notify_all();
#endif
    }

    void FfmpegVideo::read_diagnostics()
    {
#ifndef _WIN32
        constexpr std::size_t kept = 4096;
        std::array<char, 1024> bytes{};
        while (true)
        {
            const auto count = read(errors_, bytes.data(), bytes.size());
            if (count <= 0)
            {
                break;
            }
            std::lock_guard lock(mutex_);
            diagnostics_.append(bytes.data(), static_cast<std::size_t>(count));
            // Only the end matters: it holds the error that stopped the stream.
            if (diagnostics_.size() > kept)
            {
                diagnostics_.erase(0, diagnostics_.size() - kept);
            }
        }
#endif
    }

    bool FfmpegVideo::finished() const
    {
        std::lock_guard lock(mutex_);
        return finished_;
    }

    std::string FfmpegVideo::diagnostics() const
    {
        std::lock_guard lock(mutex_);
        return diagnostics_;
    }

    bool FfmpegVideo::wait_for_frame(
        std::uint64_t& sequence, std::string& jpeg, std::chrono::milliseconds timeout
    )
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(
                lock,
                timeout,
                [&]
                {
                    return sequence_ != sequence || finished_;
                }
            ) ||
            sequence_ == sequence)
        {
            return false;
        }
        sequence = sequence_;
        jpeg = frame_;
        return !jpeg.empty();
    }

    FfmpegVideo::~FfmpegVideo()
    {
#ifndef _WIN32
        if (process_ > 0)
        {
            (void)kill(process_, SIGTERM);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            int status = 0;
            while (std::chrono::steady_clock::now() < deadline)
            {
                const auto waited = waitpid(process_, &status, WNOHANG);
                if (waited == process_ || (waited < 0 && errno == ECHILD))
                {
                    process_ = -1;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (process_ > 0)
            {
                (void)kill(process_, SIGKILL);
                while (waitpid(process_, &status, 0) < 0 && errno == EINTR)
                {
                }
                process_ = -1;
            }
        }
        if (output_ >= 0)
        {
            close(output_);
            output_ = -1;
        }
        if (reader_.joinable())
        {
            reader_.join();
        }
        if (errors_ >= 0)
        {
            close(errors_);
            errors_ = -1;
        }
        if (diagnostic_reader_.joinable())
        {
            diagnostic_reader_.join();
        }
#endif
    }

    void serve_mjpeg(
        VideoFrameSource& source, ResponseStream& output, const std::function<bool()>& finished
    )
    {
        // About two full-HD JPEGs: enough to keep a LAN busy, too little to
        // queue seconds of video for a client on a slow link.
        output.limit_send_buffer(256 * 1024);
        if (!output.begin(200, "multipart/x-mixed-replace; boundary=wlonvif-frame"))
        {
            return;
        }
        std::uint64_t sequence{};
        std::string frame;
        auto written = std::chrono::steady_clock::now();
        while (!output.stopping())
        {
            std::string next;
            if (source.wait_for_frame(sequence, next, std::chrono::seconds(1)))
            {
                frame = std::move(next);
            }
            else if (finished && finished())
            {
                return;
            }
            else if (frame.empty() ||
                     std::chrono::steady_clock::now() - written < std::chrono::seconds(2))
            {
                continue;
            }
            const std::string header =
                "--wlonvif-frame\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                std::to_string(frame.size()) + "\r\n\r\n";
            if (!output.write(header) || !output.write(frame) || !output.write("\r\n"))
            {
                return;
            }
            written = std::chrono::steady_clock::now();
        }
    }
} // namespace wlonvif::examples::web

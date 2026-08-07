#include "ffmpeg_video.hpp"
#include <array>
#include <cerrno>
#include <cstring>
#include <vector>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
extern char** environ;
#endif

namespace wlonvif::examples::ptz_web
{
    namespace
    {
        Error launch_error(std::string message)
        {
            return Error{ErrorCategory::network, OperationId::get_stream_uri, std::move(message)};
        }
    }

    Result<std::shared_ptr<FfmpegVideo>> FfmpegVideo::start(std::string input_url,
                                                            std::string executable)
    {
        if (input_url.empty()) return launch_error("FFmpeg input URL is empty");
        auto result = std::shared_ptr<FfmpegVideo>(new FfmpegVideo);
#ifdef _WIN32
        (void)executable;
        return launch_error("the simple FFmpeg bridge is not yet available on Windows");
#else
        int pipe_descriptors[2]{};
        if (pipe(pipe_descriptors) != 0) return launch_error("unable to create FFmpeg output pipe");
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, pipe_descriptors[1], STDOUT_FILENO);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addclose(&actions, pipe_descriptors[0]);
        posix_spawn_file_actions_addclose(&actions, pipe_descriptors[1]);
        std::vector<std::string> arguments{executable, "-nostdin", "-hide_banner", "-loglevel",
                                          "error"};
        const bool rtsp = input_url.starts_with("rtsp://") || input_url.starts_with("rtsps://");
        if (rtsp)
            arguments.insert(arguments.end(), {"-rtsp_transport", "tcp"});
        else if (!input_url.starts_with("http://") && !input_url.starts_with("https://"))
            arguments.push_back("-re");
        arguments.insert(arguments.end(), {"-i", std::move(input_url), "-map", "0:v:0", "-an",
            "-vf", "fps=15", "-q:v", "5", "-f", "image2pipe", "-vcodec", "mjpeg", "pipe:1"});
        std::vector<char*> argv;
        for (auto& argument : arguments) argv.push_back(argument.data());
        argv.push_back(nullptr);
        const int status = posix_spawnp(&result->process_, executable.c_str(), &actions, nullptr,
                                        argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(pipe_descriptors[1]);
        if (status != 0)
        {
            close(pipe_descriptors[0]);
            return launch_error("unable to launch FFmpeg: " + std::string(std::strerror(status)));
        }
        result->output_ = pipe_descriptors[0];
        result->reader_ = std::thread([pointer = result.get()] { pointer->read_frames(); });
        return result;
#endif
    }

    void FfmpegVideo::read_frames()
    {
#ifndef _WIN32
        constexpr std::size_t maximum_frame = 4 * 1024 * 1024;
        std::string pending;
        std::array<char, 65536> bytes{};
        while (true)
        {
            const auto count = read(output_, bytes.data(), bytes.size());
            if (count <= 0) break;
            pending.append(bytes.data(), static_cast<std::size_t>(count));
            while (true)
            {
                const auto start = pending.find("\xff\xd8");
                if (start == std::string::npos) { pending.clear(); break; }
                const auto end = pending.find("\xff\xd9", start + 2);
                if (end == std::string::npos)
                {
                    if (start) pending.erase(0, start);
                    if (pending.size() > maximum_frame) pending.clear();
                    break;
                }
                const auto length = end + 2 - start;
                if (length <= maximum_frame)
                {
                    std::lock_guard lock(mutex_);
                    frame_.assign(pending, start, length);
                    ++sequence_;
                    changed_.notify_all();
                }
                pending.erase(0, end + 2);
            }
        }
#endif
    }

    bool FfmpegVideo::wait_for_frame(std::uint64_t& sequence, std::string& jpeg,
                                     std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, timeout, [&] { return sequence_ != sequence; })) return false;
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
                while (waitpid(process_, &status, 0) < 0 && errno == EINTR) {}
                process_ = -1;
            }
        }
        if (output_ >= 0) { close(output_); output_ = -1; }
        if (reader_.joinable()) reader_.join();
#endif
    }
}

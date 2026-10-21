/**
 * @file recorder.cpp
 * @brief FFmpeg stream-copy recordings: start, watch, and stop cleanly.
 */

#include "recorder.hpp"

#include <cerrno>
#include <cstring>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace wlonvif::examples::explorer
{
    namespace
    {
        Error recording_error(std::string message)
        {
            return Error{ErrorCategory::internal, OperationId::get_stream_uri, std::move(message)};
        }
    } // namespace

    Result<std::shared_ptr<Recording>> Recording::start(
        const std::string& executable,
        const std::string& input,
        std::filesystem::path output,
        std::chrono::seconds duration
    )
    {
#ifdef _WIN32
        (void)executable;
        (void)input;
        (void)output;
        (void)duration;
        return recording_error("recording is not yet available on Windows");
#else
        auto recording = std::shared_ptr<Recording>(new Recording);
        recording->output_ = std::move(output);
        recording->duration_ = duration;
        recording->started_ = std::chrono::system_clock::now();
        const auto log = recording->output_.string() + ".log";
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(
            &actions, STDERR_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600
        );
        std::vector<std::string> arguments{
            executable, "-nostdin", "-hide_banner", "-loglevel", "warning"
        };
        const bool generated = input.starts_with("lavfi:");
        if (input.starts_with("rtsp://") || input.starts_with("rtsps://"))
        {
            arguments.insert(arguments.end(), {"-rtsp_transport", "tcp"});
        }
        if (generated)
        {
            arguments.insert(arguments.end(), {"-re", "-f", "lavfi"});
        }
        arguments.insert(
            arguments.end(),
            {"-i",
             generated ? input.substr(6) : input,
             "-t",
             std::to_string(duration.count()),
             "-map",
             "0"}
        );
        // Stream copy keeps the camera's own encoding; Matroska accepts H.264,
        // H.265, MJPEG, and the common audio codecs without remuxing errors. A
        // generated source is raw video, so it is compressed instead.
        if (generated)
        {
            arguments.insert(arguments.end(), {"-c:v", "mjpeg", "-q:v", "5"});
        }
        else
        {
            arguments.insert(arguments.end(), {"-c", "copy"});
        }
        arguments.insert(arguments.end(), {"-f", "matroska", "-y", recording->output_.string()});
        std::vector<char*> argv;
        for (auto& argument : arguments)
        {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        const int status = posix_spawnp(
            &recording->process_, executable.c_str(), &actions, nullptr, argv.data(), environ
        );
        posix_spawn_file_actions_destroy(&actions);
        if (status != 0)
        {
            return recording_error(
                "unable to launch FFmpeg: " + std::string(std::strerror(status))
            );
        }
        return recording;
#endif
    }

    void Recording::reap(bool wait)
    {
#ifndef _WIN32
        if (finished_ || process_ <= 0)
        {
            return;
        }
        int status = 0;
        pid_t waited = 0;
        do
        {
            waited = waitpid(process_, &status, wait ? 0 : WNOHANG);
        } while (waited < 0 && errno == EINTR);
        if (waited == process_ || (waited < 0 && errno == ECHILD))
        {
            finished_ = true;
            status_ = waited == process_ && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
#else
        (void)wait;
#endif
    }

    bool Recording::active()
    {
        std::lock_guard lock(mutex_);
        reap(false);
        return !finished_;
    }

    int Recording::exit_status()
    {
        std::lock_guard lock(mutex_);
        reap(false);
        return finished_ ? status_ : -1;
    }

    void Recording::stop()
    {
#ifndef _WIN32
        std::lock_guard lock(mutex_);
        reap(false);
        if (finished_)
        {
            return;
        }
        // SIGINT makes FFmpeg write its trailer, leaving a playable file.
        (void)kill(process_, SIGINT);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!finished_ && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            reap(false);
        }
        if (!finished_)
        {
            (void)kill(process_, SIGKILL);
            reap(true);
        }
#endif
    }

    Recording::~Recording()
    {
        stop();
    }
} // namespace wlonvif::examples::explorer

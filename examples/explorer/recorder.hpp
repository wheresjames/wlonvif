#pragma once

/**
 * @file recorder.hpp
 * @brief Record a camera stream to a file with FFmpeg, without re-encoding.
 */

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <wlonvif/wlonvif.hpp>

namespace wlonvif::examples::explorer
{
    /// One FFmpeg stream-copy recording running in the background.
    class Recording
    {
      public:
        /**
         * @brief Start recording @p input to @p output for @p duration.
         *
         * FFmpeg's diagnostic output goes to @p output with `.log` appended, so
         * a failed recording can be explained afterwards.
         */
        static Result<std::shared_ptr<Recording>> start(
            const std::string& executable,
            const std::string& input,
            std::filesystem::path output,
            std::chrono::seconds duration
        );
        ~Recording();
        Recording(const Recording&) = delete;
        Recording& operator=(const Recording&) = delete;

        /// Whether FFmpeg is still running.
        bool active();
        /// FFmpeg's exit status once finished; -1 while running or when killed.
        int exit_status();
        /// Stop the recording early; FFmpeg finalizes the file it has written.
        void stop();
        const std::filesystem::path& output() const
        {
            return output_;
        }
        std::chrono::system_clock::time_point started() const
        {
            return started_;
        }
        std::chrono::seconds duration() const
        {
            return duration_;
        }

      private:
        Recording() = default;
        void reap(bool wait);
        std::mutex mutex_;
        std::filesystem::path output_;
        std::chrono::system_clock::time_point started_;
        std::chrono::seconds duration_{};
        int process_{-1};
        int status_{-1};
        bool finished_{};
    };
} // namespace wlonvif::examples::explorer

#pragma once

/**
 * @file ffmpeg_video.hpp
 * @brief Convert any FFmpeg-readable stream into a sequence of JPEG frames.
 */

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <wlonvif/wlonvif.hpp>

namespace wlonvif::examples::web
{
    /// A source of JPEG frames for an MJPEG response.
    class VideoFrameSource
    {
      public:
        virtual ~VideoFrameSource() = default;
        /// Wait for a frame newer than @p sequence; false on timeout or end of stream.
        virtual bool wait_for_frame(
            std::uint64_t& sequence, std::string& jpeg, std::chrono::milliseconds timeout
        ) = 0;
    };

    /// An FFmpeg child process decoding a stream into JPEG frames.
    class FfmpegVideo final : public VideoFrameSource
    {
      public:
        /**
         * @brief Start FFmpeg on @p input_url, producing at most @p frames_per_second JPEG frames.
         *
         * FFmpeg runs with low-delay options and stamps frames with their arrival
         * time, so it needs FFmpeg 5.1 or later (`-fps_mode`).
         * `lavfi:FILTER` selects a generated FFmpeg test source instead of a stream.
         */
        static Result<std::shared_ptr<FfmpegVideo>>
        start(std::string input_url, std::string executable = "ffmpeg", int frames_per_second = 15);
        ~FfmpegVideo() override;
        bool wait_for_frame(std::uint64_t&, std::string&, std::chrono::milliseconds) override;
        /// Whether FFmpeg has stopped producing output.
        bool finished() const;
        /// The end of FFmpeg's diagnostic output, which explains a failed stream.
        std::string diagnostics() const;
        FfmpegVideo(const FfmpegVideo&) = delete;
        FfmpegVideo& operator=(const FfmpegVideo&) = delete;

      private:
        FfmpegVideo() = default;
        void read_frames();
        void read_diagnostics();
        mutable std::mutex mutex_;
        std::condition_variable changed_;
        std::string frame_;
        std::string diagnostics_;
        std::uint64_t sequence_{};
        std::chrono::steady_clock::duration interval_{};
        bool finished_{};
        std::thread reader_;
        std::thread diagnostic_reader_;
        int output_{-1};
        int errors_{-1};
#ifdef _WIN32
        void* process_{};
#else
        int process_{-1};
#endif
    };
    class ResponseStream;

    /**
     * @brief Send @p source to a browser as a multipart MJPEG response.
     *
     * Only whole JPEG parts are written: browsers abandon an image whose stream
     * starts with anything but a part, and some cameras take seconds to deliver
     * the first frame. While no new frame arrives, the last one is repeated
     * every two seconds, which is also how a vanished client is noticed.
     *
     * @param finished Optional check that ends the response once the source stops.
     */
    void serve_mjpeg(
        VideoFrameSource& source, ResponseStream& output, const std::function<bool()>& finished = {}
    );
} // namespace wlonvif::examples::web

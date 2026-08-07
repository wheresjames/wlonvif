#pragma once
#include "server.hpp"
#include <condition_variable>
#include <mutex>
#include <thread>

namespace wlonvif::examples::ptz_web
{
    class FfmpegVideo final : public VideoFrameSource
    {
      public:
        static Result<std::shared_ptr<FfmpegVideo>> start(std::string input_url,
                                                          std::string executable = "ffmpeg");
        ~FfmpegVideo() override;
        bool wait_for_frame(std::uint64_t&, std::string&,
                            std::chrono::milliseconds) override;
        FfmpegVideo(const FfmpegVideo&) = delete;
        FfmpegVideo& operator=(const FfmpegVideo&) = delete;

      private:
        FfmpegVideo() = default;
        void read_frames();
        std::mutex mutex_;
        std::condition_variable changed_;
        std::string frame_;
        std::uint64_t sequence_{};
        std::thread reader_;
        int output_{-1};
#ifdef _WIN32
        void* process_{};
#else
        int process_{-1};
#endif
    };
}

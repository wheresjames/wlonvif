#include "ffmpeg_video.hpp"
#include <iostream>
using namespace wlonvif::examples::ptz_web;
#define CHECK(x) do { if (!(x)) { std::cerr << "check failed: " #x << '\n'; return 1; } } while (0)
int main(int argc, char** argv)
{
    CHECK(argc == 2);
#ifdef _WIN32
    CHECK(!FfmpegVideo::start("unused", argv[1]));
#else
    auto source = FfmpegVideo::start("unused", argv[1]);
    CHECK(source);
    std::uint64_t sequence{};
    std::string jpeg;
    CHECK(source.value()->wait_for_frame(sequence, jpeg, std::chrono::seconds(2)));
    CHECK(jpeg.starts_with("\xff\xd8") && jpeg.ends_with("\xff\xd9"));
#endif
    std::cout << "FFmpeg video bridge tests passed\n";
}

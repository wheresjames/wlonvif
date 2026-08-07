#pragma once
#include "protocol.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace wlonvif::examples::ptz_web
{
    class VideoFrameSource
    {
      public:
        virtual ~VideoFrameSource() = default;
        virtual bool wait_for_frame(std::uint64_t& sequence, std::string& jpeg,
                                    std::chrono::milliseconds timeout) = 0;
    };
    struct WebServerConfig
    {
        std::string address{"127.0.0.1"};
        std::uint16_t port{8080};
        bool allow_remote{};
        std::string certificate_file;
        std::string private_key_file;
        std::size_t maximum_connections{16};
        std::shared_ptr<VideoFrameSource> video;
        std::string ui_configuration{"{\"profiles\":[],\"selected\":\"\"}"};
        ProtocolConfig protocol;
    };
    using CommandHandler = std::function<Result<std::string>(const WebCommand&)>;
    using DisconnectHandler = std::function<void(std::string_view)>;

    class WebServer
    {
      public:
        WebServer(WebServerConfig, CommandHandler, DisconnectHandler);
        Result<void> run(std::atomic_bool& stop_requested);
      private:
        struct Impl;
        std::shared_ptr<Impl> impl_;
    };
}

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <wlonvif/wlonvif.hpp>

namespace wlonvif::examples::ptz_web
{
    enum class CommandType { intent, stop, click, box, preset, tour, select_profile };
    struct WebCommand
    {
        CommandType type{CommandType::stop};
        std::string owner;
        std::uint64_t sequence{};
        PtzVector intent;
        ViewportPoint first;
        ViewportPoint second;
        std::chrono::milliseconds calibration_age{};
        bool invert_pan{};
        bool invert_tilt{};
        std::string preset;
        std::string profile;
        std::vector<std::string> tour_presets;
        std::chrono::milliseconds dwell{2000};
        std::size_t repeats{1};
    };
    struct ProtocolConfig
    {
        std::vector<std::string> allowed_origins;
        std::chrono::milliseconds lease{750};
        std::size_t maximum_message_bytes{4096};
        std::size_t maximum_messages_per_second{60};
        std::size_t maximum_sessions{32};
    };
    class ControlProtocol
    {
      public:
        explicit ControlProtocol(ProtocolConfig config);
        Result<std::string> create_session(
            std::string_view origin,
            std::chrono::steady_clock::time_point now,
            std::string session_token
        );
        Result<WebCommand> handle(
            std::string_view session_token,
            std::string_view origin,
            std::string_view message,
            std::chrono::steady_clock::time_point now
        );
        std::optional<std::string> disconnect(std::string_view session_token);
        std::vector<std::string> expire(std::chrono::steady_clock::time_point now);
        void revoke_all();
        bool origin_allowed(std::string_view origin) const;
        bool session_valid(std::string_view session_token,
                           std::chrono::steady_clock::time_point now) const;

      private:
        struct Session
        {
            std::string owner;
            std::uint64_t sequence{};
            std::chrono::steady_clock::time_point expires{};
            std::chrono::steady_clock::time_point rate_window{};
            std::size_t rate_count{};
        };
        ProtocolConfig config_;
        std::unordered_map<std::string, Session> sessions_;
        std::uint64_t next_owner_{};
    };
}

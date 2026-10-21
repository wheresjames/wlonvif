#pragma once

/**
 * @file protocol.hpp
 * @brief Sessions and messages of the browser control socket, without any networking.
 *
 * Kept free of sockets and threads so it can be tested and fuzzed directly.
 * The server calls it under a mutex.
 */

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
    /// What a control message asks for.
    enum class CommandType
    {
        intent,        ///< Move with this velocity; repeated about every 100 ms while held.
        stop,          ///< Stop now.
        click,         ///< Centre the view on a clicked point.
        box,           ///< Centre and zoom to a dragged box.
        preset,        ///< Go to a preset.
        tour,          ///< Visit presets in turn.
        select_profile ///< Control another media profile.
    };
    /// A validated control message. Only the fields of its type are meaningful.
    struct WebCommand
    {
        CommandType type{CommandType::stop};
        std::string owner;        ///< Session that sent it, such as `web-1`.
        std::uint64_t sequence{}; ///< Strictly increasing within the session.
        PtzVector intent;         ///< intent: each axis within -1 to 1.
        ViewportPoint first;      ///< click: the point; box: one corner.
        ViewportPoint second;     ///< box: the opposite corner.
        std::chrono::milliseconds calibration_age{};
        bool invert_pan{};  ///< click and box: mirror horizontal movement.
        bool invert_tilt{}; ///< click and box: mirror vertical movement.
        std::string preset;
        std::string profile;
        std::vector<std::string> tour_presets;
        std::chrono::milliseconds dwell{2000}; ///< tour: time at each preset.
        std::size_t repeats{1};                ///< tour: rounds.
    };
    /// Limits that bound what one browser can make the server do.
    struct ProtocolConfig
    {
        /// Exact `Origin` values allowed to open a session, such as `http://127.0.0.1:8080`.
        std::vector<std::string> allowed_origins;
        /// A session without a valid message for this long expires, and its movement stops.
        std::chrono::milliseconds lease{750};
        std::size_t maximum_message_bytes{4096};
        std::size_t maximum_messages_per_second{60};
        std::size_t maximum_sessions{32};
    };
    /// The sessions of the browsers connected to one server.
    class ControlProtocol
    {
      public:
        explicit ControlProtocol(ProtocolConfig config);
        /// Start a session for a page from @p origin; returns the owner name for its commands.
        Result<std::string> create_session(
            std::string_view origin,
            std::chrono::steady_clock::time_point now,
            std::string session_token
        );
        /// Validate one message from a session; any failure leaves the session unchanged.
        Result<WebCommand> handle(
            std::string_view session_token,
            std::string_view origin,
            std::string_view message,
            std::chrono::steady_clock::time_point now
        );
        /// End a session; returns its owner, so the caller can stop what it started.
        std::optional<std::string> disconnect(std::string_view session_token);
        /// End sessions whose lease ran out; returns their owners.
        std::vector<std::string> expire(std::chrono::steady_clock::time_point now);
        /// End every session, at shutdown.
        void revoke_all();
        bool origin_allowed(std::string_view origin) const;
        bool session_valid(
            std::string_view session_token, std::chrono::steady_clock::time_point now
        ) const;

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
} // namespace wlonvif::examples::ptz_web

#pragma once

/**
 * @file camera_control.hpp
 * @brief Turns validated browser commands into camera movement, one owner at a time.
 *
 * Several browsers may be connected, but only one controls the camera at a
 * time: the first to send manual input becomes the *owner* until it stops,
 * disconnects, or its session expires. Automatic operations (preset tours,
 * click-to-centre, box zoom) run until manual input or Stop interrupts them.
 *
 * Every command bumps a generation counter, so a long operation, such as a
 * tour running on its own thread, can see that something newer has replaced it.
 */

#include "protocol.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <wlonvif/wlonvif.hpp>

namespace wlonvif::examples::ptz_web
{
    /// What one media profile supports, as the browser shows it.
    struct BrowserProfile
    {
        std::string token;
        std::string name;
        bool continuous_pan_tilt{};
        bool continuous_zoom{};
        bool relative_pan_tilt{}; ///< Needed for click-to-centre.
        bool relative_zoom{};     ///< Needed for box zoom.
        bool presets{};           ///< At least one preset exists.
    };

    struct CameraControlConfig
    {
        std::shared_ptr<PtzClient> ptz; ///< Null in diagnostic mode: nothing is sent.
        std::shared_ptr<PtzController> controller;
        std::string profile; ///< Profile controlled at start.
        std::vector<BrowserProfile> profiles;
        ViewportCalibration calibration;
        int timeout_seconds{10};
        /// Set when the program is stopping; ends a running tour.
        const std::atomic_bool* stop_requested{};
    };

    class CameraControl
    {
      public:
        explicit CameraControl(CameraControlConfig config);
        ~CameraControl();
        CameraControl(const CameraControl&) = delete;
        CameraControl& operator=(const CameraControl&) = delete;

        /// Carry out one command; the reply is telemetry JSON for the browser.
        Result<std::string> handle(const WebCommand& command);

        /// A browser went away: if it was moving the camera, stop.
        void disconnected(std::string_view owner);

        /// End any tour and close the controller, which sends a final Stop.
        Result<void> shutdown();

      private:
        Result<std::string> select_profile(const WebCommand& command);
        Result<std::string> manual_intent(
            const WebCommand& command,
            const std::shared_ptr<PtzController>& controller,
            const BrowserProfile& capabilities
        );
        Result<std::string> start_tour(
            const WebCommand& command,
            std::uint64_t generation,
            const std::shared_ptr<PtzController>& controller,
            const std::string& profile
        );
        Result<std::string> viewport_move(
            const WebCommand& command,
            const std::shared_ptr<PtzController>& controller,
            const std::string& profile,
            const BrowserProfile& capabilities
        );
        void join_tour();
        CallOptions call() const;

        CameraControlConfig config_;
        ViewportMapper mapper_;

        std::mutex mutex_;                          ///< Guards the fields below.
        std::shared_ptr<PtzController> controller_; ///< Replaced when the profile changes.
        std::string profile_;
        std::string active_owner_; ///< Session in control; empty when nobody is.
        bool automatic_active_{};  ///< A tour or a click/box move is running.
        bool switching_profile_{}; ///< Commands are refused while the controller is replaced.

        std::atomic_uint64_t generation_{}; ///< Bumped by every command that changes the movement.

        // Tours run on their own thread. Running one inside the handler would occupy
        // the connection's read loop for its whole duration, so that browser's Stop
        // and even its disconnect would go unread until the tour ended.
        std::mutex tour_mutex_;
        std::thread tour_thread_;
    };

    /// Telemetry JSON for the browser: commands sent, stops, latency, and zoom.
    std::string telemetry(const std::shared_ptr<PtzController>& controller);
} // namespace wlonvif::examples::ptz_web

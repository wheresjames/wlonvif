#pragma once

/**
 * @file input_model.hpp
 * @brief Keyboard and gamepad state turned into a PTZ velocity, with no SDL in sight.
 *
 * Kept separate from SDL so the rules that decide when the camera moves can be
 * tested directly: dead zones, inversion, precision mode, and above all the
 * safety rules that stop the camera when the window loses focus or the
 * gamepad is unplugged.
 */

#include <array>
#include <string>
#include <string_view>
#include <wlonvif/wlonvif.hpp>

namespace wlonvif::examples::ptz_control
{
    /// Input tuning, loaded from --config; --print-default-config shows these defaults.
    struct InputConfig
    {
        int version{1};
        double dead_zone{0.10}; ///< Stick movement below this counts as zero (worn sticks drift).
        double precision_scale{0.25}; ///< Speed while the precision key is held.
        double keyboard_speed{0.75};  ///< Speed of a key press, from 0 to 1.
        bool invert_pan{false};
        bool invert_tilt{true}; ///< SDL reports stick Y as negative upward.
        bool invert_zoom{true};
        // SDL gamepad axis names: leftx, lefty, rightx, righty, left_trigger, right_trigger.
        std::string pan_axis{"leftx"};
        std::string tilt_axis{"lefty"};
        std::string zoom_axis{"righty"};
    };

    /// Parse a version 1 configuration; a field with the wrong type or range is an error.
    Result<InputConfig> parse_config(std::string_view json);
    /// @p config as the JSON parse_config() reads.
    std::string config_json(const InputConfig& config);

    enum class Key
    {
        left,
        right,
        up,
        down,
        zoom_in,
        zoom_out,
        precision
    };

    /// The controls' current state; the main loop reads intent() every frame.
    class InputModel
    {
      public:
        explicit InputModel(InputConfig config = {});
        void key(Key key, bool down);
        /// Raw stick positions, -1 to 1; the dead zone is applied here.
        void axes(double pan, double tilt, double zoom);
        /// Losing focus releases every key and requests a Stop.
        void focus(bool focused);
        /// A gamepad going away releases its axes and requests a Stop.
        void device(bool connected);
        void emergency_stop();
        /// The velocity to send now; zero while the window is not focused.
        PtzVector intent() const;
        /// True once after a Stop was requested, so it is sent exactly once.
        bool consume_stop();
        bool precision() const noexcept;
        bool focused() const noexcept
        {
            return focused_;
        }
        bool connected() const noexcept
        {
            return connected_;
        }

      private:
        InputConfig config_;
        std::array<bool, 7> keys_{};
        PtzVector axes_{0.0, 0.0, 0.0};
        bool focused_{true};
        bool connected_{false};
        bool stop_pending_{false};
    };
} // namespace wlonvif::examples::ptz_control

#pragma once

#include <array>
#include <string>
#include <string_view>
#include <wlonvif/wlonvif.hpp>

namespace wlonvif::examples::ptz_control
{
    struct InputConfig
    {
        int version{1};
        double dead_zone{0.10};
        double precision_scale{0.25};
        double keyboard_speed{0.75};
        bool invert_pan{false};
        bool invert_tilt{true};
        bool invert_zoom{true};
        std::string pan_axis{"leftx"};
        std::string tilt_axis{"lefty"};
        std::string zoom_axis{"righty"};
    };

    Result<InputConfig> parse_config(std::string_view json);
    std::string config_json(const InputConfig& config);

    enum class Key { left, right, up, down, zoom_in, zoom_out, precision };

    class InputModel
    {
      public:
        explicit InputModel(InputConfig config = {});
        void key(Key key, bool down);
        void axes(double pan, double tilt, double zoom);
        void focus(bool focused);
        void device(bool connected);
        void emergency_stop();
        PtzVector intent() const;
        bool consume_stop();
        bool precision() const noexcept;
        bool focused() const noexcept { return focused_; }
        bool connected() const noexcept { return connected_; }

      private:
        InputConfig config_;
        std::array<bool, 7> keys_{};
        PtzVector axes_{0.0, 0.0, 0.0};
        bool focused_{true};
        bool connected_{false};
        bool stop_pending_{false};
    };
}

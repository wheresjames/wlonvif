/**
 * @file input_model.cpp
 * @brief Configuration parsing and the input-to-velocity rules.
 */

#include "input_model.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <regex>
#include <sstream>
#include <utility>

namespace wlonvif::examples::ptz_control
{
    namespace
    {
        // The configuration is a small flat object, so fields are found with
        // regular expressions rather than a JSON library.

        std::optional<double> number(std::string_view json, std::string_view name)
        {
            const std::regex expression(
                "\\\"" + std::string(name) + "\\\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)"
            );
            std::cmatch match;
            return std::regex_search(json.begin(), json.end(), match, expression)
                       ? std::optional<double>{std::stod(match[1].str())}
                       : std::nullopt;
        }
        std::optional<bool> boolean(std::string_view json, std::string_view name)
        {
            const std::regex expression("\\\"" + std::string(name) + "\\\"\\s*:\\s*(true|false)");
            std::cmatch match;
            return std::regex_search(json.begin(), json.end(), match, expression)
                       ? std::optional<bool>{match[1].str() == "true"}
                       : std::nullopt;
        }
        std::optional<std::string> text(std::string_view json, std::string_view name)
        {
            const std::regex expression(
                "\\\"" + std::string(name) + "\\\"\\s*:\\s*\\\"([^\\\"]+)\\\""
            );
            std::cmatch match;
            return std::regex_search(json.begin(), json.end(), match, expression)
                       ? std::optional<std::string>{match[1].str()}
                       : std::nullopt;
        }
        double clamp(double value)
        {
            return std::clamp(value, -1.0, 1.0);
        }
        bool known_axis(std::string_view value)
        {
            return value == "leftx" || value == "lefty" || value == "rightx" || value == "righty" ||
                   value == "left_trigger" || value == "right_trigger";
        }
        bool mentions(std::string_view json, std::string_view name)
        {
            return json.find("\"" + std::string(name) + "\"") != std::string_view::npos;
        }
    } // namespace

    Result<InputConfig> parse_config(std::string_view json)
    {
        InputConfig result;
        if (json.find('{') == std::string_view::npos || json.rfind('}') == std::string_view::npos)
        {
            return Error{
                ErrorCategory::invalid_argument,
                OperationId::continuous_move,
                "PTZ input configuration must be a JSON object"
            };
        }
        const auto version = number(json, "version");
        if (!version || *version != 1)
        {
            return Error{
                ErrorCategory::invalid_argument,
                OperationId::continuous_move,
                "PTZ input configuration requires version 1"
            };
        }
        result.version = 1;
        const auto dead_zone = number(json, "dead_zone");
        const auto precision = number(json, "precision_scale");
        const auto keyboard = number(json, "keyboard_speed");
        const auto invert_pan = boolean(json, "invert_pan");
        const auto invert_tilt = boolean(json, "invert_tilt");
        const auto invert_zoom = boolean(json, "invert_zoom");
        const auto pan_axis = text(json, "pan_axis");
        const auto tilt_axis = text(json, "tilt_axis");
        const auto zoom_axis = text(json, "zoom_axis");
        // A field that is present but did not match its pattern has the wrong type.
        if ((mentions(json, "dead_zone") && !dead_zone) ||
            (mentions(json, "precision_scale") && !precision) ||
            (mentions(json, "keyboard_speed") && !keyboard) ||
            (mentions(json, "invert_pan") && !invert_pan) ||
            (mentions(json, "invert_tilt") && !invert_tilt) ||
            (mentions(json, "invert_zoom") && !invert_zoom) ||
            (mentions(json, "pan_axis") && !pan_axis) ||
            (mentions(json, "tilt_axis") && !tilt_axis) ||
            (mentions(json, "zoom_axis") && !zoom_axis))
        {
            return Error{
                ErrorCategory::invalid_argument,
                OperationId::continuous_move,
                "PTZ input configuration field has the wrong type"
            };
        }
        if (dead_zone)
        {
            result.dead_zone = *dead_zone;
        }
        if (precision)
        {
            result.precision_scale = *precision;
        }
        if (keyboard)
        {
            result.keyboard_speed = *keyboard;
        }
        if (invert_pan)
        {
            result.invert_pan = *invert_pan;
        }
        if (invert_tilt)
        {
            result.invert_tilt = *invert_tilt;
        }
        if (invert_zoom)
        {
            result.invert_zoom = *invert_zoom;
        }
        if (pan_axis)
        {
            result.pan_axis = *pan_axis;
        }
        if (tilt_axis)
        {
            result.tilt_axis = *tilt_axis;
        }
        if (zoom_axis)
        {
            result.zoom_axis = *zoom_axis;
        }
        if (!std::isfinite(result.dead_zone) || result.dead_zone < 0 || result.dead_zone >= 1 ||
            !std::isfinite(result.precision_scale) || result.precision_scale <= 0 ||
            result.precision_scale > 1 || !std::isfinite(result.keyboard_speed) ||
            result.keyboard_speed <= 0 || result.keyboard_speed > 1 ||
            !known_axis(result.pan_axis) || !known_axis(result.tilt_axis) ||
            !known_axis(result.zoom_axis))
        {
            return Error{
                ErrorCategory::invalid_argument,
                OperationId::continuous_move,
                "invalid PTZ input configuration value"
            };
        }
        return result;
    }

    std::string config_json(const InputConfig& config)
    {
        std::ostringstream output;
        output << std::boolalpha << std::setprecision(4)
               << "{\n  \"version\": 1,\n  \"dead_zone\": " << config.dead_zone
               << ",\n  \"precision_scale\": " << config.precision_scale
               << ",\n  \"keyboard_speed\": " << config.keyboard_speed
               << ",\n  \"invert_pan\": " << config.invert_pan
               << ",\n  \"invert_tilt\": " << config.invert_tilt
               << ",\n  \"invert_zoom\": " << config.invert_zoom << ",\n  \"pan_axis\": \""
               << config.pan_axis << "\",\n  \"tilt_axis\": \"" << config.tilt_axis
               << "\",\n  \"zoom_axis\": \"" << config.zoom_axis << "\"\n}\n";
        return output.str();
    }

    InputModel::InputModel(InputConfig config) : config_(std::move(config))
    {
    }
    void InputModel::key(Key value, bool down)
    {
        keys_[static_cast<std::size_t>(value)] = down;
    }
    void InputModel::axes(double pan, double tilt, double zoom)
    {
        const auto filter = [this](double value)
        {
            if (!std::isfinite(value) || std::abs(value) <= config_.dead_zone)
            {
                return 0.0;
            }
            return clamp(value);
        };
        axes_ = {filter(pan), filter(tilt), filter(zoom)};
    }
    void InputModel::focus(bool focused)
    {
        focused_ = focused;
        if (!focused)
        {
            keys_.fill(false);
            axes_ = {0.0, 0.0, 0.0};
            stop_pending_ = true;
        }
    }
    void InputModel::device(bool connected)
    {
        if (connected_ && !connected)
        {
            axes_ = {0.0, 0.0, 0.0};
            stop_pending_ = true;
        }
        connected_ = connected;
    }
    void InputModel::emergency_stop()
    {
        stop_pending_ = true;
    }
    PtzVector InputModel::intent() const
    {
        if (!focused_)
        {
            return {0.0, 0.0, 0.0};
        }
        const auto down = [this](Key key)
        {
            return keys_[static_cast<std::size_t>(key)];
        };
        const double precision = down(Key::precision) ? config_.precision_scale : 1.0;
        double pan =
            axes_.pan.value_or(0) + config_.keyboard_speed * (down(Key::right) - down(Key::left));
        // Default SDL Y axes are negative upward. Keep keyboard and gamepad signs
        // identical before applying the shared inversion settings.
        double tilt =
            axes_.tilt.value_or(0) + config_.keyboard_speed * (down(Key::down) - down(Key::up));
        double zoom = axes_.zoom.value_or(0) +
                      config_.keyboard_speed * (down(Key::zoom_out) - down(Key::zoom_in));
        pan *= config_.invert_pan ? -1 : 1;
        tilt *= config_.invert_tilt ? -1 : 1;
        zoom *= config_.invert_zoom ? -1 : 1;
        return {clamp(pan * precision), clamp(tilt * precision), clamp(zoom * precision)};
    }
    bool InputModel::consume_stop()
    {
        return std::exchange(stop_pending_, false);
    }
    bool InputModel::precision() const noexcept
    {
        return keys_[static_cast<std::size_t>(Key::precision)];
    }
} // namespace wlonvif::examples::ptz_control

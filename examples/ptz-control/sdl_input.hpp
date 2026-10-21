#pragma once

/**
 * @file sdl_input.hpp
 * @brief SDL3 window, keyboard, and gamepad or joystick input for ptz-control.
 */

#include "input_model.hpp"

#include <SDL3/SDL.h>
#include <string>
#include <vector>

namespace wlonvif::examples::ptz_control
{
    /// What the window shows for one frame.
    struct UiFrame
    {
        PtzVector intent;
        std::optional<PtzControllerState> controller;
        bool diagnostic{};
        bool calibrating{};
        bool precision{};
        bool stop_triggered{};
        // Extremes seen per axis in --calibrate mode, to set dead zones and inversion.
        std::array<double, 3> calibration_min{};
        std::array<double, 3> calibration_max{};
    };

    /// Owns SDL: the window, the selected input device, and event handling.
    class SdlInput
    {
      public:
        /// @p selector picks a device by name fragment or id; @p raw skips SDL's gamepad mapping.
        SdlInput(InputConfig config, std::string selector, bool raw);
        ~SdlInput();
        SdlInput(const SdlInput&) = delete;
        SdlInput& operator=(const SdlInput&) = delete;
        /// Start SDL; without a window only device listing works.
        Result<void> open(bool create_window);
        /// Apply pending events to @p model; false when the user asked to quit (Esc or close).
        bool poll(InputModel& model);
        /// Draw the intent, controller state, and Stop feedback.
        void render(const InputModel& model, const UiFrame& frame);
        /// Connected joysticks and gamepads, for --list-inputs.
        std::vector<std::string> devices() const;
        /// The device in use, or "keyboard only".
        std::string active_name() const;
        PtzVector raw_axes() const noexcept
        {
            return raw_axes_;
        }

      private:
        void select_device();
        void close_device(InputModel* model = nullptr);
        double axis(std::string_view name) const;
        InputConfig config_;
        std::string selector_;
        bool raw_{};
        bool initialized_{};
        SDL_Window* window_{};
        SDL_Renderer* renderer_{};
        SDL_Gamepad* gamepad_{};
        SDL_Joystick* joystick_{};
        SDL_JoystickID active_id_{};
        PtzVector raw_axes_{0.0, 0.0, 0.0};
    };
} // namespace wlonvif::examples::ptz_control

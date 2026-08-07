#pragma once

#include "input_model.hpp"

#include <SDL3/SDL.h>
#include <string>
#include <vector>

namespace wlonvif::examples::ptz_control
{
    struct UiFrame
    {
        PtzVector intent;
        std::optional<PtzControllerState> controller;
        bool diagnostic{};
        bool calibrating{};
        bool precision{};
        bool stop_triggered{};
        std::array<double, 3> calibration_min{};
        std::array<double, 3> calibration_max{};
    };

    class SdlInput
    {
      public:
        SdlInput(InputConfig config, std::string selector, bool raw);
        ~SdlInput();
        SdlInput(const SdlInput&) = delete;
        SdlInput& operator=(const SdlInput&) = delete;
        Result<void> open(bool create_window);
        bool poll(InputModel& model);
        void render(const InputModel& model, const UiFrame& frame);
        std::vector<std::string> devices() const;
        std::string active_name() const;
        PtzVector raw_axes() const noexcept { return raw_axes_; }

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
}

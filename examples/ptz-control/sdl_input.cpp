#include "sdl_input.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace wlonvif::examples::ptz_control
{
    namespace
    {
        std::string lower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }
        int raw_axis(std::string_view name)
        {
            if (name == "leftx") return 0;
            if (name == "lefty") return 1;
            if (name == "rightx") return 2;
            if (name == "righty") return 3;
            if (name == "left_trigger") return 4;
            return 5;
        }
        double normalized(Sint16 value)
        {
            return std::clamp(static_cast<double>(value) / 32767.0, -1.0, 1.0);
        }
        const char* mode_name(PtzControllerMode mode)
        {
            switch (mode)
            {
            case PtzControllerMode::idle: return "IDLE";
            case PtzControllerMode::manual: return "MANUAL";
            case PtzControllerMode::stopping: return "STOPPING";
            case PtzControllerMode::degraded: return "DEGRADED";
            case PtzControllerMode::closed: return "CLOSED";
            }
            return "UNKNOWN";
        }
        std::optional<Key> mapped_key(SDL_Scancode key)
        {
            switch (key)
            {
            case SDL_SCANCODE_A: case SDL_SCANCODE_LEFT: return Key::left;
            case SDL_SCANCODE_D: case SDL_SCANCODE_RIGHT: return Key::right;
            case SDL_SCANCODE_W: case SDL_SCANCODE_UP: return Key::up;
            case SDL_SCANCODE_S: case SDL_SCANCODE_DOWN: return Key::down;
            case SDL_SCANCODE_E: return Key::zoom_in;
            case SDL_SCANCODE_Q: return Key::zoom_out;
            case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return Key::precision;
            default: return std::nullopt;
            }
        }
    }

    SdlInput::SdlInput(InputConfig config, std::string selector, bool raw)
        : config_(std::move(config)), selector_(lower(std::move(selector))), raw_(raw) {}
    SdlInput::~SdlInput()
    {
        close_device();
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
        if (initialized_) SDL_Quit();
    }
    Result<void> SdlInput::open(bool create_window)
    {
        SDL_InitFlags flags = SDL_INIT_EVENTS | SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK;
        if (create_window) flags |= SDL_INIT_VIDEO;
        if (!SDL_Init(flags))
            return Error{ErrorCategory::internal, OperationId::continuous_move, SDL_GetError()};
        initialized_ = true;
        if (create_window)
        {
            window_ = SDL_CreateWindow("wlonvif PTZ control", 800, 520, SDL_WINDOW_RESIZABLE);
            if (!window_)
                return Error{ErrorCategory::internal, OperationId::continuous_move, SDL_GetError()};
            renderer_ = SDL_CreateRenderer(window_, nullptr);
            if (!renderer_)
                return Error{ErrorCategory::internal, OperationId::continuous_move, SDL_GetError()};
        }
        select_device();
        return {};
    }
    std::vector<std::string> SdlInput::devices() const
    {
        std::vector<std::string> result;
        int count = 0;
        SDL_JoystickID* ids = SDL_GetJoysticks(&count);
        for (int index = 0; index < count; ++index)
        {
            const char* name = SDL_GetJoystickNameForID(ids[index]);
            result.push_back(std::to_string(ids[index]) + " " + (name ? name : "unknown") +
                (SDL_IsGamepad(ids[index]) ? " [gamepad]" : " [raw]"));
        }
        SDL_free(ids);
        return result;
    }
    std::string SdlInput::active_name() const
    {
        const char* name = active_id_ ? SDL_GetJoystickNameForID(active_id_) : nullptr;
        return name ? name : "keyboard only";
    }
    void SdlInput::select_device()
    {
        if (gamepad_ || joystick_) return;
        int count = 0;
        SDL_JoystickID* ids = SDL_GetJoysticks(&count);
        for (int index = 0; index < count; ++index)
        {
            const char* candidate = SDL_GetJoystickNameForID(ids[index]);
            if (!selector_.empty() &&
                lower(candidate ? candidate : "").find(selector_) == std::string::npos &&
                std::to_string(ids[index]) != selector_) continue;
            if (!raw_ && SDL_IsGamepad(ids[index])) gamepad_ = SDL_OpenGamepad(ids[index]);
            else joystick_ = SDL_OpenJoystick(ids[index]);
            if (gamepad_ || joystick_) { active_id_ = ids[index]; break; }
        }
        SDL_free(ids);
    }
    void SdlInput::close_device(InputModel* model)
    {
        if (gamepad_) SDL_CloseGamepad(gamepad_);
        if (joystick_) SDL_CloseJoystick(joystick_);
        gamepad_ = nullptr;
        joystick_ = nullptr;
        active_id_ = 0;
        if (model) model->device(false);
    }
    double SdlInput::axis(std::string_view name) const
    {
        if (gamepad_)
        {
            const auto mapped = SDL_GetGamepadAxisFromString(std::string(name).c_str());
            return mapped == SDL_GAMEPAD_AXIS_INVALID ? 0 : normalized(SDL_GetGamepadAxis(gamepad_, mapped));
        }
        return joystick_ ? normalized(SDL_GetJoystickAxis(joystick_, raw_axis(name))) : 0;
    }
    bool SdlInput::poll(InputModel& model)
    {
        SDL_Event event{};
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_EVENT_QUIT) return false;
            if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) model.focus(false);
            if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) model.focus(true);
            if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP)
            {
                if (event.key.scancode == SDL_SCANCODE_ESCAPE && event.key.down) return false;
                if (event.key.scancode == SDL_SCANCODE_SPACE && event.key.down) model.emergency_stop();
                if (auto key = mapped_key(event.key.scancode)) model.key(*key, event.key.down);
            }
            if ((event.type == SDL_EVENT_GAMEPAD_REMOVED || event.type == SDL_EVENT_JOYSTICK_REMOVED) &&
                event.gdevice.which == active_id_) close_device(&model);
            if (event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_JOYSTICK_ADDED ||
                event.type == SDL_EVENT_GAMEPAD_REMAPPED) select_device();
            if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP)
            {
                if (event.gbutton.which == active_id_ &&
                    event.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)
                    model.key(Key::precision, event.gbutton.down);
                if (event.gbutton.which == active_id_ && event.gbutton.down &&
                    event.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) model.emergency_stop();
            }
            if (event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN ||
                event.type == SDL_EVENT_JOYSTICK_BUTTON_UP)
            {
                if (event.jbutton.which == active_id_ && event.jbutton.button == 4)
                    model.key(Key::precision, event.jbutton.down);
                if (event.jbutton.which == active_id_ && event.jbutton.down &&
                    event.jbutton.button == 6) model.emergency_stop();
            }
        }
        model.device(gamepad_ || joystick_);
        raw_axes_ = {axis(config_.pan_axis), axis(config_.tilt_axis), axis(config_.zoom_axis)};
        model.axes(raw_axes_.pan.value_or(0), raw_axes_.tilt.value_or(0), raw_axes_.zoom.value_or(0));
        return true;
    }

    void SdlInput::render(const InputModel& model, const UiFrame& frame)
    {
        if (!renderer_) return;
        int width = 0, height = 0;
        SDL_GetRenderOutputSize(renderer_, &width, &height);
        const auto color = [this](Uint8 r, Uint8 g, Uint8 b)
        {
            SDL_SetRenderDrawColor(renderer_, r, g, b, 255);
        };
        const auto text = [this](float x, float y, const std::string& value)
        {
            SDL_RenderDebugText(renderer_, x, y, value.c_str());
        };
        color(18, 22, 28);
        SDL_RenderClear(renderer_);

        color(230, 235, 240);
        text(20, 18, "WLONVIF NATIVE PTZ CONTROL");
        color(frame.diagnostic ? 255 : 105, frame.diagnostic ? 190 : 220, 100);
        text(20, 40, frame.diagnostic ? "DIAGNOSTIC MODE - CAMERA DISCONNECTED" :
                                       "LIVE CAMERA CONTROL - MOVEMENT ENABLED");
        color(model.focused() ? 80 : 255, model.focused() ? 210 : 80, 90);
        text(20, 64, std::string("FOCUS: ") +
            (model.focused() ? "ACTIVE" : "LOST - STOP REQUESTED"));
        color(model.connected() ? 80 : 220, model.connected() ? 210 : 170, 90);
        text(20, 84, std::string("INPUT: ") + active_name() +
            (model.connected() ? (raw_ ? " [RAW]" : " [GAMEPAD]") : " [KEYBOARD]"));
        if (frame.stop_triggered)
        {
            color(255, 55, 55);
            SDL_FRect warning{std::max(20.0f, static_cast<float>(width) - 300), 18, 280, 72};
            SDL_RenderFillRect(renderer_, &warning);
            color(255, 255, 255);
            text(warning.x + 18, 48, "EMERGENCY STOP REQUESTED");
        }

        const float panel_x = 30;
        const float panel_y = 125;
        const float panel_size = std::clamp(static_cast<float>(height) - 250.0f, 120.0f, 250.0f);
        color(60, 70, 82);
        SDL_FRect pad{panel_x, panel_y, panel_size, panel_size};
        SDL_RenderRect(renderer_, &pad);
        SDL_RenderLine(renderer_, panel_x + panel_size / 2, panel_y,
                       panel_x + panel_size / 2, panel_y + panel_size);
        SDL_RenderLine(renderer_, panel_x, panel_y + panel_size / 2,
                       panel_x + panel_size, panel_y + panel_size / 2);
        const float dead = static_cast<float>(config_.dead_zone) * panel_size / 2;
        color(95, 75, 45);
        SDL_FRect dead_zone{panel_x + panel_size / 2 - dead, panel_y + panel_size / 2 - dead,
                           dead * 2, dead * 2};
        SDL_RenderRect(renderer_, &dead_zone);
        const float pan = static_cast<float>(frame.intent.pan.value_or(0));
        const float tilt = static_cast<float>(frame.intent.tilt.value_or(0));
        color(70, 210, 255);
        SDL_FRect cursor{panel_x + (pan + 1) * panel_size / 2 - 6,
                         panel_y + (1 - tilt) * panel_size / 2 - 6, 12, 12};
        SDL_RenderFillRect(renderer_, &cursor);
        color(230, 235, 240);
        text(panel_x, panel_y + panel_size + 12, "PAN / TILT");

        const float gauge_x = panel_x + panel_size + 55;
        color(60, 70, 82);
        SDL_FRect gauge{gauge_x, panel_y, 32, panel_size};
        SDL_RenderRect(renderer_, &gauge);
        const float zoom = static_cast<float>(frame.intent.zoom.value_or(0));
        color(150, 110, 255);
        SDL_FRect zoom_bar{gauge_x + 3, panel_y + (1 - zoom) * panel_size / 2,
                           26, zoom * panel_size / 2};
        if (zoom_bar.h < 0) { zoom_bar.y += zoom_bar.h; zoom_bar.h = -zoom_bar.h; }
        SDL_RenderFillRect(renderer_, &zoom_bar);
        color(230, 235, 240);
        text(gauge_x - 3, panel_y + panel_size + 12, "ZOOM");

        char values[192]{};
        std::snprintf(values, sizeof(values),
            "INTENT  PAN %+.3f   TILT %+.3f   ZOOM %+.3f",
            frame.intent.pan.value_or(0), frame.intent.tilt.value_or(0),
            frame.intent.zoom.value_or(0));
        text(20, 420, values);
        std::snprintf(values, sizeof(values),
            "RAW     PAN %+.3f   TILT %+.3f   ZOOM %+.3f   PRECISION %s",
            raw_axes_.pan.value_or(0), raw_axes_.tilt.value_or(0), raw_axes_.zoom.value_or(0),
            frame.precision ? "ON" : "OFF");
        text(20, 440, values);

        if (frame.calibrating)
        {
            std::snprintf(values, sizeof(values),
                "RANGE   P %.2f..%.2f  T %.2f..%.2f  Z %.2f..%.2f",
                frame.calibration_min[0], frame.calibration_max[0],
                frame.calibration_min[1], frame.calibration_max[1],
                frame.calibration_min[2], frame.calibration_max[2]);
            color(255, 200, 90);
            text(20, 460, values);
        }
        else if (frame.controller)
        {
            std::snprintf(values, sizeof(values),
                "MODE %s   COMMANDS %zu   STOPS %zu   INPUT->CMD %lldus   REQUEST %lldus",
                mode_name(frame.controller->mode), frame.controller->commands_sent,
                frame.controller->stops_sent,
                static_cast<long long>(frame.controller->last_input_to_command.count()),
                static_cast<long long>(frame.controller->last_request_latency.count()));
            color(150, 220, 160);
            text(20, 460, values);
        }
        color(170, 180, 190);
        text(20, static_cast<float>(height - 30),
             "WASD/ARROWS MOVE   Q/E ZOOM   SHIFT PRECISION   SPACE STOP   ESC EXIT");
        SDL_RenderPresent(renderer_);
    }
}

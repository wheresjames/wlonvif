/**
 * @file camera_control.cpp
 * @brief Manual control, profile changes, presets, tours, and click/box moves.
 */

#include "camera_control.hpp"

#include <algorithm>
#include <cmath>
#include <example_support.hpp>
#include <iostream>

namespace wlonvif::examples::ptz_web
{
    std::string telemetry(const std::shared_ptr<PtzController>& controller)
    {
        if (!controller)
        {
            return R"({"mode":"diagnostic"})";
        }
        const auto state = controller->state();
        std::string zoom{"null"};
        if (state.reported && state.reported->position_reliable && state.reported->position.zoom &&
            std::isfinite(*state.reported->position.zoom))
        {
            zoom = std::to_string(*state.reported->position.zoom);
        }
        return "{\"commands\":" + std::to_string(state.commands_sent) +
               ",\"stops\":" + std::to_string(state.stops_sent) +
               ",\"request_us\":" + std::to_string(state.last_request_latency.count()) +
               ",\"reported_zoom\":" + zoom + "}";
    }

    CameraControl::CameraControl(CameraControlConfig config)
        : config_(std::move(config)), mapper_(config_.calibration), controller_(config_.controller),
          profile_(config_.profile)
    {
    }

    CameraControl::~CameraControl()
    {
        ++generation_;
        join_tour();
    }

    CallOptions CameraControl::call() const
    {
        return examples::call_options(config_.timeout_seconds);
    }

    void CameraControl::join_tour()
    {
        std::thread finished;
        {
            std::lock_guard lock(tour_mutex_);
            finished = std::move(tour_thread_);
        }
        if (finished.joinable())
        {
            finished.join();
        }
    }

    Result<std::string> CameraControl::handle(const WebCommand& command)
    {
        // Take a consistent snapshot of what is being controlled.
        std::shared_ptr<PtzController> controller;
        std::string profile;
        BrowserProfile capabilities;
        {
            std::lock_guard lock(mutex_);
            if (switching_profile_)
            {
                return Error{
                    ErrorCategory::queue_overflow,
                    OperationId::continuous_move,
                    "profile selection is in progress"
                };
            }
            controller = controller_;
            profile = profile_;
            const auto found = std::ranges::find_if(
                config_.profiles,
                [&](const auto& item)
                {
                    return item.token == profile;
                }
            );
            if (found != config_.profiles.end())
            {
                capabilities = *found;
            }
        }
        if (!controller)
        {
            return telemetry(controller); // Diagnostic mode.
        }
        if (command.type == CommandType::select_profile)
        {
            return select_profile(command);
        }
        if (command.type == CommandType::intent)
        {
            return manual_intent(command, controller, capabilities);
        }

        // Everything else starts by stopping what is happening and releasing manual control.
        const auto generation = ++generation_;
        auto stopped = examples::wait_for_task(
            controller->stop(examples::safety_call_options(config_.timeout_seconds))
        );
        if (!stopped)
        {
            return stopped.error();
        }
        {
            std::lock_guard lock(mutex_);
            if (!active_owner_.empty())
            {
                (void)controller->release_manual(active_owner_);
                (void)controller->relinquish_manual(active_owner_);
                active_owner_.clear();
            }
        }

        switch (command.type)
        {
            case CommandType::stop:
                return telemetry(controller);
            case CommandType::preset:
                {
                    if (!capabilities.presets)
                    {
                        return Error{
                            ErrorCategory::unsupported,
                            OperationId::goto_preset,
                            "presets are unavailable for this profile"
                        };
                    }
                    auto result = examples::wait_for_task(
                        config_.ptz->goto_preset(profile, command.preset, call())
                    );
                    if (!result)
                    {
                        return result.error();
                    }
                    return telemetry(controller);
                }
            case CommandType::tour:
                if (!capabilities.presets)
                {
                    return Error{
                        ErrorCategory::unsupported,
                        OperationId::goto_preset,
                        "presets are unavailable for this profile"
                    };
                }
                return start_tour(command, generation, controller, profile);
            default:
                return viewport_move(command, controller, profile, capabilities);
        }
    }

    Result<std::string> CameraControl::select_profile(const WebCommand& command)
    {
        const auto selected = std::ranges::find_if(
            config_.profiles,
            [&](const auto& item)
            {
                return item.token == command.profile;
            }
        );
        if (selected == config_.profiles.end())
        {
            return Error{
                ErrorCategory::invalid_argument,
                OperationId::continuous_move,
                "unknown profile token"
            };
        }
        std::shared_ptr<PtzController> current;
        {
            std::lock_guard lock(mutex_);
            if (command.profile == profile_)
            {
                return telemetry(controller_);
            }
            // Refuse other commands until the new controller is in place.
            current = controller_;
            switching_profile_ = true;
            active_owner_.clear();
            automatic_active_ = false;
        }
        ++generation_;
        // Closing the old controller stops the camera before control moves on.
        auto stopped = examples::wait_for_task(
            current->close(examples::safety_call_options(config_.timeout_seconds))
        );
        auto replacement = stopped ? PtzController::create(config_.ptz, command.profile)
                                   : Result<std::shared_ptr<PtzController>>{stopped.error()};
        {
            std::lock_guard lock(mutex_);
            if (replacement)
            {
                controller_ = replacement.value();
                profile_ = command.profile;
            }
            switching_profile_ = false;
        }
        if (!replacement)
        {
            return replacement.error();
        }
        return telemetry(replacement.value());
    }

    Result<std::string> CameraControl::manual_intent(
        const WebCommand& command,
        const std::shared_ptr<PtzController>& controller,
        const BrowserProfile& capabilities
    )
    {
        // The browser repeats its current intent every 100 ms, including an
        // all-zero one while idle. Only actual manual input may cancel an
        // automatic operation; the idle heartbeat must not end every tour.
        const auto idle = [](const std::optional<double>& value)
        {
            return !value || std::abs(*value) < 1e-6;
        };
        if (idle(command.intent.pan) && idle(command.intent.tilt) && idle(command.intent.zoom))
        {
            std::lock_guard lock(mutex_);
            if (automatic_active_)
            {
                return telemetry(controller);
            }
        }

        // Real input interrupts a tour or click/box move.
        ++generation_;
        bool interrupt = false;
        {
            std::lock_guard lock(mutex_);
            interrupt = automatic_active_;
            automatic_active_ = false;
            if (interrupt)
            {
                active_owner_.clear();
            }
        }
        if (interrupt)
        {
            auto stopped = examples::wait_for_task(controller->stop(call()));
            if (!stopped)
            {
                return stopped.error();
            }
        }

        // The first session to move becomes the owner; others are refused.
        {
            std::lock_guard lock(mutex_);
            if (active_owner_.empty())
            {
                auto claimed = controller->claim_manual(command.owner);
                if (!claimed)
                {
                    return claimed.error();
                }
                active_owner_ = command.owner;
            }
            if (active_owner_ != command.owner)
            {
                return Error{
                    ErrorCategory::authorization,
                    OperationId::continuous_move,
                    "another browser session owns manual control"
                };
            }
        }

        // Axes the profile cannot move are zeroed rather than refused.
        auto intent = command.intent;
        if (!capabilities.continuous_pan_tilt)
        {
            intent.pan = 0.0;
            intent.tilt = 0.0;
        }
        if (!capabilities.continuous_zoom)
        {
            intent.zoom = 0.0;
        }
        auto accepted = controller->set_manual_input(command.owner, command.sequence, intent);
        if (!accepted)
        {
            return accepted.error();
        }
        return telemetry(controller);
    }

    Result<std::string> CameraControl::start_tour(
        const WebCommand& command,
        std::uint64_t generation,
        const std::shared_ptr<PtzController>& controller,
        const std::string& profile
    )
    {
        // The generation was advanced by handle(), so a previous tour is already
        // winding down; wait for it before starting the replacement.
        join_tour();
        if (generation != generation_.load())
        {
            return Error{ErrorCategory::cancelled, OperationId::goto_preset, "tour superseded"};
        }
        {
            std::lock_guard lock(mutex_);
            automatic_active_ = true;
            active_owner_ = command.owner;
        }
        std::lock_guard lock(tour_mutex_);
        tour_thread_ = std::thread(
            [this, command, generation, profile]
            {
                const auto interrupted = [&]
                {
                    return generation != generation_.load() ||
                           (config_.stop_requested && config_.stop_requested->load());
                };
                for (std::size_t repeat = 0; repeat < command.repeats && !interrupted(); ++repeat)
                {
                    for (const auto& preset : command.tour_presets)
                    {
                        if (interrupted())
                        {
                            break;
                        }
                        auto result = examples::wait_for_task(
                            config_.ptz->goto_preset(profile, preset, call())
                        );
                        if (!result)
                        {
                            std::cerr << "Tour stopped: " << result.error().message << '\n';
                            repeat = command.repeats;
                            break;
                        }
                        // Stay at the preset, checking often so Stop is prompt.
                        const auto end = std::chrono::steady_clock::now() + command.dwell;
                        while (std::chrono::steady_clock::now() < end && !interrupted())
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(25));
                        }
                    }
                }
                // Hand control back, unless a newer command already took it.
                std::lock_guard lock(mutex_);
                if (generation == generation_.load())
                {
                    if (active_owner_ == command.owner)
                    {
                        active_owner_.clear();
                    }
                    automatic_active_ = false;
                }
            }
        );
        return telemetry(controller);
    }

    Result<std::string> CameraControl::viewport_move(
        const WebCommand& command,
        const std::shared_ptr<PtzController>& controller,
        const std::string& profile,
        const BrowserProfile& capabilities
    )
    {
        if (!capabilities.relative_pan_tilt)
        {
            return Error{
                ErrorCategory::unsupported,
                OperationId::relative_move,
                "relative pan/tilt is unavailable for this profile"
            };
        }
        if (command.type == CommandType::box && !capabilities.relative_zoom)
        {
            return Error{
                ErrorCategory::unsupported,
                OperationId::relative_move,
                "relative zoom is unavailable for this profile"
            };
        }

        // The current zoom changes how far a pixel is, when the camera reports it.
        std::optional<double> normalized_zoom;
        const auto state = controller->state();
        if (state.reported && state.reported->position_reliable)
        {
            normalized_zoom = state.reported->position.zoom;
        }
        auto movement =
            command.type == CommandType::click
                ? mapper_.click_to_center(command.first, command.calibration_age, normalized_zoom)
                : mapper_.box_zoom(
                      command.first, command.second, command.calibration_age, normalized_zoom
                  );
        if (!movement)
        {
            return movement.error();
        }
        // Some mounts turn the image over; the browser can mirror the moves to match.
        auto& translation = movement.value().translation;
        if (command.invert_pan && translation.pan)
        {
            translation.pan = -*translation.pan;
        }
        if (command.invert_tilt && translation.tilt)
        {
            translation.tilt = -*translation.tilt;
        }

        {
            std::lock_guard lock(mutex_);
            automatic_active_ = true;
        }
        auto moved =
            examples::wait_for_task(config_.ptz->relative_move(profile, translation, call()));
        {
            std::lock_guard lock(mutex_);
            automatic_active_ = false;
        }
        if (!moved)
        {
            return moved.error();
        }
        return "{\"calibrated\":" + std::string(movement.value().calibrated ? "true" : "false") +
               "}";
    }

    void CameraControl::disconnected(std::string_view owner)
    {
        std::shared_ptr<PtzController> controller;
        bool owns = false;
        {
            std::lock_guard lock(mutex_);
            controller = controller_;
            owns = active_owner_ == owner;
            if (owns)
            {
                active_owner_.clear();
                automatic_active_ = false;
            }
        }
        if (owns && controller)
        {
            ++generation_;
            (void)examples::wait_for_task(
                controller->stop(examples::safety_call_options(config_.timeout_seconds))
            );
            (void)controller->release_manual(owner);
            (void)controller->relinquish_manual(owner);
        }
    }

    Result<void> CameraControl::shutdown()
    {
        ++generation_;
        join_tour();
        std::shared_ptr<PtzController> controller;
        {
            std::lock_guard lock(mutex_);
            controller = controller_;
        }
        if (!controller)
        {
            return {};
        }
        auto closed = examples::wait_for_task(
            controller->close(examples::safety_call_options(config_.timeout_seconds))
        );
        if (!closed)
        {
            return closed.error();
        }
        return {};
    }
} // namespace wlonvif::examples::ptz_web

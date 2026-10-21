/**
 * @file ptz_api.cpp
 * @brief The PTZ tab: movement, presets, preset tours, and orientation.
 *
 * Holding a movement button makes the page send an *intent* about every 150 ms
 * until it is released. Two modes turn intents into camera commands:
 *
 * - **Direct** (default): one ContinuousMove per press or change of speed,
 *   and Stop on release. Repeats only renew a lease; the watchdog thread stops
 *   the camera when the repeats stop arriving, for example after a crashed tab.
 * - **Smooth**: a PtzController ramps the speed and refreshes it. Some cameras
 *   restart their motors on every command and grind audibly in this mode.
 */

#include "implementation.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace wlonvif::examples::explorer
{
    namespace
    {
        /// Whether two velocities differ enough to be worth a new command.
        bool differs(const PtzVector& left, const PtzVector& right)
        {
            const auto axis = [](const std::optional<double>& a, const std::optional<double>& b)
            {
                return std::abs(a.value_or(0) - b.value_or(0)) > 0.02;
            };
            return axis(left.pan, right.pan) || axis(left.tilt, right.tilt) ||
                   axis(left.zoom, right.zoom);
        }

        bool is_zero(const PtzVector& value)
        {
            const auto still = [](const std::optional<double>& axis)
            {
                return std::abs(axis.value_or(0)) < 0.01;
            };
            return still(value.pan) && still(value.tilt) && still(value.zoom);
        }

        PtzVector vector_from(const json::object& body)
        {
            PtzVector value;
            value.pan = number_field(body, "pan");
            value.tilt = number_field(body, "tilt");
            value.zoom = number_field(body, "zoom");
            return value;
        }

        /// Success without data, or the failure; for commands whose answer is not shown.
        template <class T> Reply done(const Result<T>& result)
        {
            return result ? success() : failure(result.error());
        }
    } // namespace

    Reply Explorer::Implementation::ptz_view(
        Camera& camera, DeviceSession& session, const std::string& profile
    )
    {
        auto ptz = session.ptz();
        if (!ptz)
        {
            return failure(ptz.error());
        }
        if (profile.empty())
        {
            return rejection(400, "profile is required");
        }
        auto& client = *ptz.value();
        json::object output;
        output["spaces"] =
            section(run(camera.id, "GetConfigurationOptions", client.spaces(profile, call())));
        output["status"] = section(run(camera.id, "GetStatus", client.status(profile, call())));
        output["presets"] =
            section(run(camera.id, "GetPresets", client.get_presets(profile, call())));
        output["tours"] =
            section(run(camera.id, "GetPresetTours", client.get_preset_tours(profile, call())));
        output["configuration"] =
            section(run(camera.id, "GetConfiguration", client.get_configuration(profile, call())));
        return success(output);
    }

    Reply Explorer::Implementation::ptz_action(
        Camera& camera, DeviceSession& session, const std::string& action, const json::object& body
    )
    {
        const auto profile = text_field(body, "profile").value_or("");
        if (profile.empty())
        {
            return rejection(400, "profile is required");
        }
        auto ptz = session.ptz();
        if (!ptz)
        {
            return failure(ptz.error());
        }
        auto& client = *ptz.value();
        const auto safety = examples::safety_call_options(5);

        if (action == "intent")
        {
            auto value = vector_from(body);
            for (auto* axis : {&value.pan, &value.tilt, &value.zoom})
            {
                if (axis->has_value())
                {
                    **axis = std::clamp(**axis, -1.0, 1.0);
                }
            }
            const auto mode = text_field(body, "mode").value_or("direct");
            if (mode == "direct")
            {
                return direct_intent(camera, session, profile, value);
            }
            if (mode != "smooth")
            {
                return rejection(400, "mode must be direct or smooth");
            }
            auto active = controller(camera, session, profile);
            if (!active)
            {
                return failure(active.error());
            }
            return done(active.value()->set_manual_input(value));
        }

        if (action == "stop")
        {
            // Stop whichever mode is moving, then send Stop regardless.
            std::shared_ptr<PtzController> active;
            {
                std::lock_guard lock(camera.ptz_mutex);
                if (const auto found = camera.controllers.find(profile);
                    found != camera.controllers.end())
                {
                    active = found->second;
                }
                if (const auto found = camera.direct.find(profile); found != camera.direct.end())
                {
                    found->second.moving = false;
                }
            }
            if (active)
            {
                (void)examples::wait_for_task(active->stop(safety));
            }
            return done(run(camera.id, "Stop", client.emergency_stop(profile, safety)));
        }

        if (action == "relative")
        {
            return done(run(
                camera.id, "RelativeMove", client.relative_move(profile, vector_from(body), call())
            ));
        }
        if (action == "absolute")
        {
            return done(
                run(camera.id,
                    "AbsoluteMove",
                    client.absolute_move(profile, vector_from(body), std::nullopt, call()))
            );
        }

        if (action == "preset")
        {
            const auto operation = text_field(body, "operation").value_or("");
            const auto token = text_field(body, "token").value_or("");
            if (operation == "goto")
            {
                return done(
                    run(camera.id, "GotoPreset", client.goto_preset(profile, token, call()))
                );
            }
            if (operation == "remove")
            {
                return done(
                    run(camera.id, "RemovePreset", client.remove_preset(profile, token, call()))
                );
            }
            if (operation == "save")
            {
                // Without a token this creates a preset; with one it moves it here.
                const auto name = text_field(body, "name").value_or("");
                auto saved =
                    run(camera.id, "SetPreset", client.set_preset(profile, name, token, call()));
                return saved ? success(json::value(saved.value())) : failure(saved.error());
            }
            return rejection(400, "operation must be goto, save, or remove");
        }

        if (action == "tour")
        {
            const auto operation = text_field(body, "operation").value_or("");
            const auto token = text_field(body, "token").value_or("");
            if (operation == "create")
            {
                PresetTour tour;
                tour.name = text_field(body, "name").value_or("Tour");
                tour.auto_start = flag_field(body, "auto_start").value_or(false);
                if (const auto spots = body.find("spots");
                    spots != body.end() && spots->value().is_array())
                {
                    for (const auto& spot : spots->value().as_array())
                    {
                        if (!spot.is_object())
                        {
                            continue;
                        }
                        const auto& fields = spot.as_object();
                        const auto stay =
                            static_cast<long long>(number_field(fields, "stay").value_or(0));
                        tour.spots.push_back(
                            {text_field(fields, "preset").value_or(""), std::chrono::seconds(stay)}
                        );
                    }
                }
                if (tour.spots.empty())
                {
                    return failure(invalid("a tour needs at least one preset"));
                }
                // ONVIF creates an empty tour first, then fills it in.
                auto created =
                    run(camera.id, "CreatePresetTour", client.create_preset_tour(profile, call()));
                if (!created)
                {
                    return failure(created.error());
                }
                tour.token = created.value();
                auto modified = run(
                    camera.id, "ModifyPresetTour", client.modify_preset_tour(profile, tour, call())
                );
                if (!modified)
                {
                    // An empty tour is useless, so do not leave it behind.
                    (void)run(
                        camera.id,
                        "RemovePresetTour",
                        client.remove_preset_tour(profile, tour.token, call())
                    );
                    return failure(modified.error());
                }
                return success(json::value(tour.token));
            }
            if (operation == "remove")
            {
                return done(run(
                    camera.id, "RemovePresetTour", client.remove_preset_tour(profile, token, call())
                ));
            }
            std::optional<PresetTourOperation> kind;
            if (operation == "start")
            {
                kind = PresetTourOperation::start;
            }
            else if (operation == "stop")
            {
                kind = PresetTourOperation::stop;
            }
            else if (operation == "pause")
            {
                kind = PresetTourOperation::pause;
            }
            if (!kind)
            {
                return rejection(400, "operation must be create, start, stop, pause, or remove");
            }
            return done(
                run(camera.id,
                    "OperatePresetTour",
                    client.operate_preset_tour(profile, token, *kind, call()))
            );
        }

        if (action == "direction")
        {
            PtzDirection direction{text_field(body, "eflip"), text_field(body, "reverse")};
            return done(
                run(camera.id,
                    "SetConfiguration",
                    client.set_direction(profile, direction, true, call()))
            );
        }
        return rejection(404, "unknown PTZ action");
    }

    // ---- Direct mode -----------------------------------------------------------

    Reply Explorer::Implementation::direct_intent(
        Camera& camera, DeviceSession& session, const std::string& profile, PtzVector value
    )
    {
        auto ptz = session.ptz();
        if (!ptz)
        {
            return failure(ptz.error());
        }
        // Decide under the lock; send after releasing it.
        bool send = false;
        bool stop = false;
        {
            std::lock_guard lock(camera.ptz_mutex);
            auto& state = camera.direct[profile];
            state.client = ptz.value();
            state.renewed = SteadyClock::now();
            if (is_zero(value))
            {
                stop = state.moving;
                state.moving = false;
            }
            else if (!state.moving || differs(state.velocity, value))
            {
                send = true;
                state.moving = true;
                state.velocity = value;
            }
        }
        if (stop)
        {
            auto stopped =
                run(camera.id,
                    "Stop",
                    ptz.value()->emergency_stop(profile, examples::safety_call_options(5)));
            return stopped ? success(json::object{{"sent", "stop"}}) : failure(stopped.error());
        }
        if (!send)
        {
            return success(json::object{{"sent", nullptr}}); // Only the lease was renewed.
        }
        auto moved =
            run(camera.id, "ContinuousMove", ptz.value()->continuous_move(profile, value, call()));
        if (!moved)
        {
            std::lock_guard lock(camera.ptz_mutex);
            camera.direct[profile].moving = false;
            return failure(moved.error());
        }
        return success(json::object{{"sent", "move"}});
    }

    void Explorer::Implementation::watch_direct_moves()
    {
        while (!watchdog_stop.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            // Collect expired moves under the locks, then stop them without.
            std::vector<std::tuple<std::string, std::string, std::shared_ptr<PtzClient>>> expired;
            const auto now = SteadyClock::now();
            for (auto& camera : all_cameras())
            {
                std::lock_guard lock(camera->ptz_mutex);
                for (auto& [profile, state] : camera->direct)
                {
                    if (state.moving && now - state.renewed > direct_lease)
                    {
                        state.moving = false;
                        expired.emplace_back(camera->id, profile, state.client);
                    }
                }
            }
            for (auto& [camera, profile, client] : expired)
            {
                (void)run(
                    camera,
                    "Stop (browser stopped renewing the move)",
                    client->emergency_stop(profile, examples::safety_call_options(5))
                );
            }
        }
    }

    // ---- Smooth mode -----------------------------------------------------------

    Result<std::shared_ptr<PtzController>> Explorer::Implementation::controller(
        Camera& camera, DeviceSession& session, const std::string& profile
    )
    {
        std::lock_guard lock(camera.ptz_mutex);
        const auto found = camera.controllers.find(profile);
        if (found != camera.controllers.end())
        {
            return found->second;
        }
        auto ptz = session.ptz();
        if (!ptz)
        {
            return ptz.error();
        }
        auto created = PtzController::create(ptz.value(), profile);
        if (!created)
        {
            return created.error();
        }
        camera.controllers.emplace(profile, created.value());
        return created.value();
    }

    void Explorer::Implementation::close_controllers(Camera& camera)
    {
        std::map<std::string, std::shared_ptr<PtzController>> controllers;
        {
            std::lock_guard lock(camera.ptz_mutex);
            controllers.swap(camera.controllers);
        }
        for (auto& [_, controller] : controllers)
        {
            (void)examples::wait_for_task(controller->close(examples::safety_call_options(5)));
        }
        // Closing the session sends the safety Stop for any direct move.
        std::lock_guard lock(camera.ptz_mutex);
        camera.direct.clear();
    }
} // namespace wlonvif::examples::explorer

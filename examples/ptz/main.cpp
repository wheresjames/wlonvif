/**
 * @file main.cpp
 * @brief wlonvif-ptz: run single PTZ operations from the command line.
 *
 *     wlonvif-ptz --ip 192.0.2.10 --profile main status
 *     wlonvif-ptz --ip 192.0.2.10 --profile main move --pan 0.3 --duration-ms 500
 * --confirm-movement
 *
 * Each command is one ONVIF operation, which makes this the tool for finding
 * out what a camera actually does: its movement spaces, presets, tours, and
 * orientation. Anything that moves the camera needs --confirm-movement. A
 * continuous move is always followed by Stop, also when interrupted by Ctrl-C.
 * For interactive control see ptz-control (gamepad) and ptz-web (browser).
 */

#include <charconv>
#include <chrono>
#include <cmath>
#include <example_support.hpp>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{
    bool parse_number(std::string_view text, double& output)
    {
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
    }

    void usage(const char* executable)
    {
        std::cout
            << "Usage: " << executable
            << " (--url DEVICE_URL | --ip ADDRESS) --profile TOKEN COMMAND [options]\n\n"
            << "Commands:\n"
            << "  status\n"
            << "  spaces\n"
            << "  presets\n"
            << "  stop\n"
            << "  move [--pan N] [--tilt N] [--zoom N] [--relative]\n"
            << "       [--duration-ms N] --confirm-movement\n"
            << "  absolute [--pan N] [--tilt N] [--zoom N] --confirm-movement\n"
            << "  goto-preset --preset TOKEN --confirm-movement\n\n"
            << "  qualify [--pan N|--tilt N|--zoom N] [--duration-ms N]\n"
            << "          --confirm-movement  (single bounded move; axes limited to +/-0.25)\n\n"
            << "Presets and tours:\n"
            << "  save-preset --name NAME [--preset TOKEN]  Save the current position\n"
            << "  remove-preset --preset TOKEN\n"
            << "  tours\n"
            << "  create-tour --name NAME --spot PRESET[:SECONDS]... [--auto-start]\n"
            << "  start-tour --tour TOKEN --confirm-movement  (keeps running until stopped)\n"
            << "  stop-tour --tour TOKEN | pause-tour --tour TOKEN | remove-tour --tour TOKEN\n\n"
            << "Configuration:\n"
            << "  config                                    Show orientation and limits\n"
            << "  set-direction [--eflip MODE] [--reverse MODE] [--no-persist]\n"
            << "      EFlip turns the image upside down for ceiling mounting\n\n"
            << "Components are normalized to [-1,+1]. A continuous move is always followed by "
               "Stop.\n"
            << "Credentials: --username NAME --password PASSWORD; environment and prompt fallbacks "
               "apply.\n";
    }

    void print_axis(std::string_view name, const std::vector<wlonvif::PtzSpace2D>& spaces)
    {
        for (const auto& space : spaces)
        {
            std::cout << name << " uri=" << (space.uri.empty() ? "(default)" : space.uri) << " x=["
                      << space.x.minimum << ',' << space.x.maximum << "] y=[" << space.y.minimum
                      << ',' << space.y.maximum << "]\n";
        }
    }

    void print_axis(std::string_view name, const std::vector<wlonvif::PtzSpace1D>& spaces)
    {
        for (const auto& space : spaces)
        {
            std::cout << name << " uri=" << (space.uri.empty() ? "(default)" : space.uri) << " x=["
                      << space.x.minimum << ',' << space.x.maximum << "]\n";
        }
    }
} // namespace

int main(int argc, char** argv)
{
    auto parsed = wlonvif::examples::parse_common_arguments(argc, argv);
    if (!parsed)
    {
        std::cerr << parsed.error().message << '\n';
        return 2;
    }
    auto common = std::move(parsed.value().common);
    std::string url, address, profile, command, preset, name, tour;
    std::vector<wlonvif::PresetTourSpot> spots;
    wlonvif::PtzDirection direction;
    bool auto_start = false, persist = true;
    wlonvif::PtzVector vector;
    bool relative = false, confirmed = false;
    std::size_t duration_ms = 500;
    for (std::size_t i = 0; i < parsed.value().remaining.size(); ++i)
    {
        const auto& argument = parsed.value().remaining[i];
        if (argument == "--help" || argument == "-h")
        {
            usage(argv[0]);
            return 0;
        }
        const bool takes_value =
            argument == "--url" || argument == "--ip" || argument == "--profile" ||
            argument == "--preset" || argument == "--pan" || argument == "--tilt" ||
            argument == "--zoom" || argument == "--duration-ms" || argument == "--name" ||
            argument == "--tour" || argument == "--spot" || argument == "--eflip" ||
            argument == "--reverse";
        if (takes_value && i + 1 >= parsed.value().remaining.size())
        {
            std::cerr << "Missing value for " << argument << '\n';
            return 2;
        }
        if (argument == "--url")
        {
            url = parsed.value().remaining[++i];
        }
        else if (argument == "--ip")
        {
            address = parsed.value().remaining[++i];
        }
        else if (argument == "--profile")
        {
            profile = parsed.value().remaining[++i];
        }
        else if (argument == "--preset")
        {
            preset = parsed.value().remaining[++i];
        }
        else if (argument == "--name")
        {
            name = parsed.value().remaining[++i];
        }
        else if (argument == "--tour")
        {
            tour = parsed.value().remaining[++i];
        }
        else if (argument == "--eflip")
        {
            direction.eflip = parsed.value().remaining[++i];
        }
        else if (argument == "--reverse")
        {
            direction.reverse = parsed.value().remaining[++i];
        }
        else if (argument == "--auto-start")
        {
            auto_start = true;
        }
        else if (argument == "--no-persist")
        {
            persist = false;
        }
        else if (argument == "--spot")
        {
            const std::string value = parsed.value().remaining[++i];
            const auto colon = value.find(':');
            double seconds = 0;
            if (value.empty() || colon == 0 ||
                (colon != std::string::npos &&
                 (!parse_number(value.substr(colon + 1), seconds) || seconds < 0)))
            {
                std::cerr << "--spot takes PRESET or PRESET:SECONDS\n";
                return 2;
            }
            spots.push_back(
                {value.substr(0, colon), std::chrono::seconds(static_cast<long long>(seconds))}
            );
        }
        else if (argument == "--duration-ms")
        {
            if (!wlonvif::examples::parse_size(parsed.value().remaining[++i], duration_ms) ||
                duration_ms == 0 || duration_ms > 5000)
            {
                std::cerr << "Duration must be between 1 and 5000 ms\n";
                return 2;
            }
        }
        else if (argument == "--pan" || argument == "--tilt" || argument == "--zoom")
        {
            double value = 0;
            if (!parse_number(parsed.value().remaining[++i], value) || value < -1 || value > 1)
            {
                std::cerr << "PTZ components must be between -1 and +1\n";
                return 2;
            }
            if (argument == "--pan")
            {
                vector.pan = value;
            }
            else if (argument == "--tilt")
            {
                vector.tilt = value;
            }
            else
            {
                vector.zoom = value;
            }
        }
        else if (argument == "--relative")
        {
            relative = true;
        }
        else if (argument == "--confirm-movement")
        {
            confirmed = true;
        }
        else if (command.empty())
        {
            command = argument;
        }
        else
        {
            std::cerr << "Unknown argument: " << argument << '\n';
            return 2;
        }
    }
    if (url.empty() == address.empty() || profile.empty() || command.empty())
    {
        usage(argv[0]);
        // Say what is missing; the usage text alone hides it.
        std::cerr << '\n'
                  << (url.empty() == address.empty() ? "Error: specify exactly one of --url or --ip"
                      : profile.empty()              ? "Error: --profile TOKEN is required"
                                        : "Error: no command given; add one such as move or status")
                  << '\n';
        return 2;
    }
    // Commands that make the camera move physically need explicit confirmation.
    const bool movement = command == "move" || command == "absolute" || command == "goto-preset" ||
                          command == "qualify" || command == "start-tour";
    const bool needs_tour = command == "start-tour" || command == "stop-tour" ||
                            command == "pause-tour" || command == "remove-tour";
    if (needs_tour && tour.empty())
    {
        std::cerr << command << " requires --tour\n";
        return 2;
    }
    if ((command == "save-preset" && name.empty() && preset.empty()) ||
        (command == "remove-preset" && preset.empty()) ||
        (command == "create-tour" && (name.empty() || spots.empty())) ||
        (command == "set-direction" && !direction.eflip && !direction.reverse))
    {
        usage(argv[0]);
        return 2;
    }
    if (movement && !confirmed)
    {
        std::cerr << "Movement requires --confirm-movement\n";
        return 2;
    }
    if ((command == "move" || command == "absolute" || command == "qualify") && !vector.pan &&
        !vector.tilt && !vector.zoom)
    {
        std::cerr << command << " requires at least one axis\n";
        return 2;
    }
    if (command == "qualify" &&
        (std::abs(vector.pan.value_or(0)) > 0.25 || std::abs(vector.tilt.value_or(0)) > 0.25 ||
         std::abs(vector.zoom.value_or(0)) > 0.25))
    {
        std::cerr << "Qualification axes are limited to [-0.25,+0.25]\n";
        return 2;
    }
    if (command == "goto-preset" && preset.empty())
    {
        std::cerr << "goto-preset requires --preset\n";
        return 2;
    }

    auto credentials = wlonvif::examples::load_credentials(common);
    if (!credentials)
    {
        std::cerr << credentials.error().message << '\n';
        return 2;
    }
    auto runtime_result = wlonvif::Runtime::create();
    if (!runtime_result)
    {
        return 1;
    }
    auto runtime = runtime_result.value();
    auto endpoint =
        wlonvif::examples::resolve_device_endpoint(url, address, runtime, common.timeout_seconds);
    if (!endpoint)
    {
        std::cerr << endpoint.error().message << '\n';
        return 1;
    }
    auto session = wlonvif::examples::connect_device(
        endpoint.value().url, runtime, credentials.value(), common.timeout_seconds
    );
    if (!session)
    {
        std::cerr << session.error().message << '\n';
        return 1;
    }
    auto ptz = session.value()->ptz();
    if (!ptz)
    {
        std::cerr << ptz.error().message << '\n';
        return 1;
    }
    const auto options = wlonvif::examples::call_options(common.timeout_seconds);
    int result_code = 0;

    if (command == "status")
    {
        auto result = wlonvif::examples::wait_for_task(ptz.value()->status(profile, options));
        if (!result)
        {
            std::cerr << result.error().message << '\n';
            result_code = 1;
        }
        else
        {
            std::cout << "Pan: " << result.value().position.pan.value_or(0)
                      << "\nTilt: " << result.value().position.tilt.value_or(0)
                      << "\nZoom: " << result.value().position.zoom.value_or(0)
                      << "\nMoving: " << (result.value().moving ? "yes" : "no")
                      << "\nPosition reliable: "
                      << (result.value().position_reliable ? "yes" : "no") << '\n';
        }
    }
    else if (command == "spaces")
    {
        auto result = wlonvif::examples::wait_for_task(ptz.value()->spaces(profile, options));
        if (!result)
        {
            std::cerr << result.error().message << '\n';
            result_code = 1;
        }
        else
        {
            print_axis("absolute-pan-tilt", result.value().absolute_pan_tilt);
            print_axis("absolute-zoom", result.value().absolute_zoom);
            print_axis("relative-pan-tilt", result.value().relative_pan_tilt);
            print_axis("relative-zoom", result.value().relative_zoom);
            print_axis("continuous-pan-tilt", result.value().continuous_pan_tilt);
            print_axis("continuous-zoom", result.value().continuous_zoom);
        }
    }
    else if (command == "presets")
    {
        auto result = wlonvif::examples::wait_for_task(ptz.value()->get_presets(profile, options));
        if (!result)
        {
            std::cerr << result.error().message << '\n';
            result_code = 1;
        }
        else
        {
            for (const auto& value : result.value())
            {
                std::cout << value.token << "  " << value.name << '\n';
            }
        }
    }
    else if (command == "stop")
    {
        auto result =
            wlonvif::examples::wait_for_task(ptz.value()->emergency_stop(profile, options));
        if (!result)
        {
            std::cerr << result.error().message << '\n';
            result_code = 1;
        }
    }
    else if (command == "goto-preset")
    {
        auto result =
            wlonvif::examples::wait_for_task(ptz.value()->goto_preset(profile, preset, options));
        if (!result)
        {
            std::cerr << result.error().message << '\n';
            result_code = 1;
        }
    }
    else if (command == "absolute")
    {
        auto result = wlonvif::examples::wait_for_task(
            ptz.value()->absolute_move(profile, vector, std::nullopt, options)
        );
        if (!result)
        {
            std::cerr << result.error().message << '\n';
            result_code = 1;
        }
    }
    else if (command == "move")
    {
        // A relative move ends by itself; a continuous one runs until Stop, so it
        // gets Stop after --duration-ms, or at once if Ctrl-C ended the wait.
        auto result = relative ? wlonvif::examples::wait_for_task(
                                     ptz.value()->relative_move(profile, vector, options)
                                 )
                               : wlonvif::examples::wait_for_task(
                                     ptz.value()->continuous_move(profile, vector, options)
                                 );
        if (!relative && result)
        {
            (void)wlonvif::examples::interruptible_wait(std::chrono::milliseconds(duration_ms));
        }
        if (!relative)
        {
            auto stopped =
                wlonvif::examples::wait_for_task(ptz.value()->emergency_stop(profile, options));
            if (!stopped && result)
            {
                std::cerr << stopped.error().message << '\n';
                result_code = 1;
            }
        }
        if (!result)
        {
            std::cerr << result.error().message << '\n';
            result_code = 1;
        }
    }
    else if (command == "qualify")
    {
        // One small, bounded move with timings and the position before and after,
        // to check a camera's responsiveness and whether it reports position.
        const auto qualify_options = [&]
        {
            return wlonvif::examples::call_options(common.timeout_seconds);
        };
        const auto before =
            wlonvif::examples::wait_for_task(ptz.value()->status(profile, qualify_options()));
        const auto move_started = std::chrono::steady_clock::now();
        auto moved = wlonvif::examples::wait_for_task(
            ptz.value()->continuous_move(profile, vector, qualify_options())
        );
        const auto move_completed = std::chrono::steady_clock::now();
        if (moved)
        {
            (void)wlonvif::examples::interruptible_wait(std::chrono::milliseconds(duration_ms));
        }
        const auto stop_started = std::chrono::steady_clock::now();
        auto stopped = wlonvif::examples::wait_for_task(
            ptz.value()->emergency_stop(profile, qualify_options())
        );
        const auto stop_completed = std::chrono::steady_clock::now();
        const auto after =
            wlonvif::examples::wait_for_task(ptz.value()->status(profile, qualify_options()));
        const auto milliseconds = [](auto start, auto end)
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        };
        std::cout << "move_request_ms=" << milliseconds(move_started, move_completed)
                  << "\nstop_request_ms=" << milliseconds(stop_started, stop_completed)
                  << "\nduration_ms=" << duration_ms << '\n';
        if (before)
        {
            std::cout << "before=" << before.value().position.pan.value_or(0) << ','
                      << before.value().position.tilt.value_or(0) << ','
                      << before.value().position.zoom.value_or(0)
                      << " reliable=" << (before.value().position_reliable ? "yes" : "no") << '\n';
        }
        if (after)
        {
            std::cout << "after=" << after.value().position.pan.value_or(0) << ','
                      << after.value().position.tilt.value_or(0) << ','
                      << after.value().position.zoom.value_or(0)
                      << " reliable=" << (after.value().position_reliable ? "yes" : "no") << '\n';
        }
        if (!moved || !stopped || !before || !after)
        {
            const auto& error = !moved     ? moved.error()
                                : !stopped ? stopped.error()
                                : !before  ? before.error()
                                           : after.error();
            std::cerr << error.message << '\n';
            result_code = 1;
        }
    }
    else if (command == "save-preset")
    {
        auto result = wlonvif::examples::wait_for_task(
            ptz.value()->set_preset(profile, name, preset, options)
        );
        if (!result)
        {
            std::cerr << result.error().message << '\n';
            result_code = 1;
        }
        else
        {
            std::cout << "Saved preset " << result.value() << '\n';
        }
    }
    else if (command == "remove-preset")
    {
        auto result =
            wlonvif::examples::wait_for_task(ptz.value()->remove_preset(profile, preset, options));
        if (!result)
        {
            std::cerr << result.error().message << '\n';
            result_code = 1;
        }
    }
    else if (command == "tours")
    {
        auto result =
            wlonvif::examples::wait_for_task(ptz.value()->get_preset_tours(profile, options));
        if (!result)
        {
            std::cerr << result.error().message << '\n';
            result_code = 1;
        }
        else
        {
            for (const auto& value : result.value())
            {
                const auto state = value.state == wlonvif::PresetTourState::idle      ? "idle"
                                   : value.state == wlonvif::PresetTourState::touring ? "touring"
                                   : value.state == wlonvif::PresetTourState::paused  ? "paused"
                                                                                      : "unknown";
                std::cout << value.token << "  " << value.name << "  " << state
                          << (value.auto_start ? "  auto-start" : "") << '\n';
                for (const auto& spot : value.spots)
                {
                    std::cout << "    preset " << spot.preset << "  stay " << spot.stay_time.count()
                              << "s\n";
                }
            }
        }
    }
    else if (command == "create-tour")
    {
        auto created =
            wlonvif::examples::wait_for_task(ptz.value()->create_preset_tour(profile, options));
        if (!created)
        {
            std::cerr << created.error().message << '\n';
            result_code = 1;
        }
        else
        {
            wlonvif::PresetTour value{
                created.value(), name, wlonvif::PresetTourState::idle, auto_start, spots
            };
            auto modified = wlonvif::examples::wait_for_task(
                ptz.value()->modify_preset_tour(profile, value, options)
            );
            if (!modified)
            {
                std::cerr << modified.error().message << '\n';
                // An empty tour is useless, so do not leave it behind.
                (void)wlonvif::examples::wait_for_task(
                    ptz.value()->remove_preset_tour(profile, created.value(), options)
                );
                result_code = 1;
            }
            else
            {
                std::cout << "Created tour " << created.value() << '\n';
                // Devices acknowledge a tour even when they substitute their own
                // stay times or start behaviour, so report any difference.
                auto stored = wlonvif::examples::wait_for_task(
                    ptz.value()->get_preset_tours(profile, options)
                );
                if (stored)
                {
                    for (const auto& item : stored.value())
                    {
                        if (item.token != created.value())
                        {
                            continue;
                        }
                        bool stays = item.spots.size() == spots.size();
                        for (std::size_t index = 0; stays && index < spots.size(); ++index)
                        {
                            stays = spots[index].stay_time.count() == 0 ||
                                    item.spots[index].stay_time == spots[index].stay_time;
                        }
                        if (!stays)
                        {
                            std::cerr << "note: the device chose its own stay times\n";
                        }
                        if (item.auto_start != auto_start)
                        {
                            std::cerr << "note: the device set auto-start "
                                      << (item.auto_start ? "on" : "off") << '\n';
                        }
                    }
                }
            }
        }
    }
    else if (needs_tour)
    {
        auto result = command == "remove-tour"
                          ? wlonvif::examples::wait_for_task(
                                ptz.value()->remove_preset_tour(profile, tour, options)
                            )
                          : wlonvif::examples::wait_for_task(ptz.value()->operate_preset_tour(
                                profile,
                                tour,
                                command == "start-tour"  ? wlonvif::PresetTourOperation::start
                                : command == "stop-tour" ? wlonvif::PresetTourOperation::stop
                                                         : wlonvif::PresetTourOperation::pause,
                                options
                            ));
        if (!result)
        {
            std::cerr << result.error().message << '\n';
            result_code = 1;
        }
    }
    else if (command == "config" || command == "set-direction")
    {
        if (command == "set-direction")
        {
            auto result = wlonvif::examples::wait_for_task(
                ptz.value()->set_direction(profile, direction, persist, options)
            );
            if (!result)
            {
                std::cerr << result.error().message << '\n';
                result_code = 1;
            }
        }
        auto result =
            wlonvif::examples::wait_for_task(ptz.value()->get_configuration(profile, options));
        if (!result)
        {
            std::cerr << result.error().message << '\n';
            result_code = 1;
        }
        else
        {
            const auto& value = result.value();
            const auto modes = [](const std::vector<std::string>& values)
            {
                std::string output;
                for (const auto& mode : values)
                {
                    output += (output.empty() ? "" : ", ") + mode;
                }
                return output.empty() ? std::string{} : "  (" + output + ")";
            };
            std::cout << "Configuration " << value.token << "  " << value.name << "  node "
                      << value.node_token << "\nDefault timeout " << value.default_timeout.count()
                      << "s"
                      << "\nEFlip " << value.direction.eflip.value_or("not supported")
                      << modes(value.eflip_modes) << "\nReverse "
                      << value.direction.reverse.value_or("not supported")
                      << modes(value.reverse_modes) << '\n';
            if (command == "set-direction" &&
                ((direction.eflip && value.direction.eflip != direction.eflip) ||
                 (direction.reverse && value.direction.reverse != direction.reverse)))
            {
                std::cerr << "note: the device did not store the requested direction\n";
            }
        }
    }
    else
    {
        std::cerr << "Unknown command: " << command << '\n';
        result_code = 2;
    }

    (void)runtime->shutdown();
    return result_code;
}

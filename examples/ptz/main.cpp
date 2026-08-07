/** @file main.cpp @brief Diagnose and invoke individual ONVIF PTZ operations. */

#include <charconv>
#include <chrono>
#include <cmath>
#include <example_support.hpp>
#include <iostream>
#include <thread>

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
            << "Components are normalized to [-1,+1]. A continuous move is always followed by Stop.\n"
            << "Credentials: --username NAME --password PASSWORD; environment and prompt fallbacks apply.\n";
    }

    void print_axis(std::string_view name, const std::vector<wlonvif::PtzSpace2D>& spaces)
    {
        for (const auto& space : spaces)
            std::cout << name << " uri=" << (space.uri.empty() ? "(default)" : space.uri)
                      << " x=[" << space.x.minimum << ',' << space.x.maximum << "] y=["
                      << space.y.minimum << ',' << space.y.maximum << "]\n";
    }

    void print_axis(std::string_view name, const std::vector<wlonvif::PtzSpace1D>& spaces)
    {
        for (const auto& space : spaces)
            std::cout << name << " uri=" << (space.uri.empty() ? "(default)" : space.uri)
                      << " x=[" << space.x.minimum << ',' << space.x.maximum << "]\n";
    }
}

int main(int argc, char** argv)
{
    auto parsed = wlonvif::examples::parse_common_arguments(argc, argv);
    if (!parsed) { std::cerr << parsed.error().message << '\n'; return 2; }
    auto common = std::move(parsed.value().common);
    std::string url, address, profile, command, preset;
    wlonvif::PtzVector vector;
    bool relative = false, confirmed = false;
    std::size_t duration_ms = 500;
    for (std::size_t i = 0; i < parsed.value().remaining.size(); ++i)
    {
        const auto& argument = parsed.value().remaining[i];
        if (argument == "--help" || argument == "-h") { usage(argv[0]); return 0; }
        const bool takes_value = argument == "--url" || argument == "--ip" ||
                                 argument == "--profile" || argument == "--preset" ||
                                 argument == "--pan" || argument == "--tilt" ||
                                 argument == "--zoom" || argument == "--duration-ms";
        if (takes_value && i + 1 >= parsed.value().remaining.size())
        {
            std::cerr << "Missing value for " << argument << '\n';
            return 2;
        }
        if (argument == "--url") url = parsed.value().remaining[++i];
        else if (argument == "--ip") address = parsed.value().remaining[++i];
        else if (argument == "--profile") profile = parsed.value().remaining[++i];
        else if (argument == "--preset") preset = parsed.value().remaining[++i];
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
            if (argument == "--pan") vector.pan = value;
            else if (argument == "--tilt") vector.tilt = value;
            else vector.zoom = value;
        }
        else if (argument == "--relative") relative = true;
        else if (argument == "--confirm-movement") confirmed = true;
        else if (command.empty()) command = argument;
        else { std::cerr << "Unknown argument: " << argument << '\n'; return 2; }
    }
    if (url.empty() == address.empty() || profile.empty() || command.empty())
    {
        usage(argv[0]);
        return 2;
    }
    const bool movement = command == "move" || command == "absolute" ||
                          command == "goto-preset" || command == "qualify";
    if (movement && !confirmed)
    {
        std::cerr << "Movement requires --confirm-movement\n";
        return 2;
    }
    if ((command == "move" || command == "absolute" || command == "qualify") &&
        !vector.pan && !vector.tilt && !vector.zoom)
    {
        std::cerr << command << " requires at least one axis\n";
        return 2;
    }
    if (command == "qualify" &&
        (std::abs(vector.pan.value_or(0)) > 0.25 ||
         std::abs(vector.tilt.value_or(0)) > 0.25 ||
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
    if (!credentials) { std::cerr << credentials.error().message << '\n'; return 2; }
    auto runtime_result = wlonvif::Runtime::create();
    if (!runtime_result) return 1;
    auto runtime = runtime_result.value();
    auto endpoint = wlonvif::examples::resolve_device_endpoint(
        url, address, runtime, common.timeout_seconds
    );
    if (!endpoint) { std::cerr << endpoint.error().message << '\n'; return 1; }
    auto session = wlonvif::examples::connect_device(
        endpoint.value().url, runtime, credentials.value(), common.timeout_seconds
    );
    if (!session) { std::cerr << session.error().message << '\n'; return 1; }
    auto ptz = session.value()->ptz();
    if (!ptz) { std::cerr << ptz.error().message << '\n'; return 1; }
    const auto options = wlonvif::examples::call_options(common.timeout_seconds);
    int result_code = 0;

    if (command == "status")
    {
        auto result = wlonvif::examples::wait_for_task(ptz.value()->status(profile, options));
        if (!result) { std::cerr << result.error().message << '\n'; result_code = 1; }
        else
            std::cout << "Pan: " << result.value().position.pan.value_or(0)
                      << "\nTilt: " << result.value().position.tilt.value_or(0)
                      << "\nZoom: " << result.value().position.zoom.value_or(0)
                      << "\nMoving: " << (result.value().moving ? "yes" : "no")
                      << "\nPosition reliable: " << (result.value().position_reliable ? "yes" : "no") << '\n';
    }
    else if (command == "spaces")
    {
        auto result = wlonvif::examples::wait_for_task(ptz.value()->spaces(profile, options));
        if (!result) { std::cerr << result.error().message << '\n'; result_code = 1; }
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
        if (!result) { std::cerr << result.error().message << '\n'; result_code = 1; }
        else for (const auto& value : result.value()) std::cout << value.token << "  " << value.name << '\n';
    }
    else if (command == "stop")
    {
        auto result = wlonvif::examples::wait_for_task(ptz.value()->emergency_stop(profile, options));
        if (!result) { std::cerr << result.error().message << '\n'; result_code = 1; }
    }
    else if (command == "goto-preset")
    {
        auto result = wlonvif::examples::wait_for_task(ptz.value()->goto_preset(profile, preset, options));
        if (!result) { std::cerr << result.error().message << '\n'; result_code = 1; }
    }
    else if (command == "absolute")
    {
        auto result = wlonvif::examples::wait_for_task(
            ptz.value()->absolute_move(profile, vector, std::nullopt, options)
        );
        if (!result) { std::cerr << result.error().message << '\n'; result_code = 1; }
    }
    else if (command == "move")
    {
        auto result = relative
                          ? wlonvif::examples::wait_for_task(ptz.value()->relative_move(profile, vector, options))
                          : wlonvif::examples::wait_for_task(ptz.value()->continuous_move(profile, vector, options));
        if (!relative && result)
            (void)wlonvif::examples::interruptible_wait(std::chrono::milliseconds(duration_ms));
        if (!relative)
        {
            auto stopped = wlonvif::examples::wait_for_task(ptz.value()->emergency_stop(profile, options));
            if (!stopped && result) { std::cerr << stopped.error().message << '\n'; result_code = 1; }
        }
        if (!result) { std::cerr << result.error().message << '\n'; result_code = 1; }
    }
    else if (command == "qualify")
    {
        const auto qualify_options = [&]
        {
            return wlonvif::examples::call_options(common.timeout_seconds);
        };
        const auto before = wlonvif::examples::wait_for_task(
            ptz.value()->status(profile, qualify_options())
        );
        const auto move_started = std::chrono::steady_clock::now();
        auto moved = wlonvif::examples::wait_for_task(
            ptz.value()->continuous_move(profile, vector, qualify_options())
        );
        const auto move_completed = std::chrono::steady_clock::now();
        if (moved)
            (void)wlonvif::examples::interruptible_wait(std::chrono::milliseconds(duration_ms));
        const auto stop_started = std::chrono::steady_clock::now();
        auto stopped = wlonvif::examples::wait_for_task(
            ptz.value()->emergency_stop(profile, qualify_options())
        );
        const auto stop_completed = std::chrono::steady_clock::now();
        const auto after = wlonvif::examples::wait_for_task(
            ptz.value()->status(profile, qualify_options())
        );
        const auto milliseconds = [](auto start, auto end)
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        };
        std::cout << "move_request_ms=" << milliseconds(move_started, move_completed)
                  << "\nstop_request_ms=" << milliseconds(stop_started, stop_completed)
                  << "\nduration_ms=" << duration_ms << '\n';
        if (before)
            std::cout << "before=" << before.value().position.pan.value_or(0) << ','
                      << before.value().position.tilt.value_or(0) << ','
                      << before.value().position.zoom.value_or(0)
                      << " reliable=" << (before.value().position_reliable ? "yes" : "no") << '\n';
        if (after)
            std::cout << "after=" << after.value().position.pan.value_or(0) << ','
                      << after.value().position.tilt.value_or(0) << ','
                      << after.value().position.zoom.value_or(0)
                      << " reliable=" << (after.value().position_reliable ? "yes" : "no") << '\n';
        if (!moved || !stopped || !before || !after)
        {
            const auto& error = !moved ? moved.error() : !stopped ? stopped.error()
                                      : !before ? before.error() : after.error();
            std::cerr << error.message << '\n';
            result_code = 1;
        }
    }
    else { std::cerr << "Unknown command: " << command << '\n'; result_code = 2; }

    (void)runtime->shutdown();
    return result_code;
}

#include "input_model.hpp"
#include "sdl_input.hpp"
#include "example_support.hpp"

#include <chrono>
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

using namespace wlonvif;
using namespace wlonvif::examples::ptz_control;

namespace
{
    void usage(const char* program)
    {
        std::cout
            << "Usage: " << program << " [common options] [input options]\n"
            << "  --diagnose-input             Run without a camera\n"
            << "  --list-inputs                List SDL joystick/gamepad devices\n"
            << "  --print-default-config       Print versioned JSON input configuration\n"
            << "  --config FILE                Load JSON input configuration\n"
            << "  --device NAME                Select device by case-insensitive name fragment\n"
            << "  --raw-joystick               Use raw axes instead of SDL gamepad mapping\n"
            << "  --calibrate                  Show live normalized axes without a camera\n"
            << "  --url URL | --ip ADDRESS     Camera endpoint\n"
            << "  --profile TOKEN              Media profile token\n"
            << "  --confirm-movement           Required before camera control\n\n"
            << "Keyboard: WASD/arrows pan/tilt, Q/E zoom, Shift precision, Space Stop, Esc quit\n"
            << "Gamepad: left stick pan/tilt, right stick Y zoom, left shoulder precision, Back Stop\n";
    }
    std::string read_file(const std::string& path)
    {
        std::ifstream input(path);
        std::ostringstream contents;
        contents << input.rdbuf();
        return input ? contents.str() : std::string{};
    }
    double part(const std::optional<double>& value) { return value.value_or(0); }
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::string_view(argv[i]) == "--help") { usage(argv[0]); return 0; }
        if (std::string_view(argv[i]) == "--print-default-config")
        {
            std::cout << config_json(InputConfig{});
            return 0;
        }
    }
    auto parsed = wlonvif::examples::parse_common_arguments(argc, argv);
    if (!parsed) { std::cerr << parsed.error().message << '\n'; return 2; }
    auto common = parsed.value().common;
    std::string url, address, profile, config_path, selector;
    bool diagnostic = false, list = false, raw = false, calibrate = false, confirmed = false;
    for (std::size_t i = 0; i < parsed.value().remaining.size(); ++i)
    {
        const auto& argument = parsed.value().remaining[i];
        const bool takes_value = argument == "--url" || argument == "--ip" ||
            argument == "--profile" || argument == "--config" || argument == "--device";
        if (takes_value && i + 1 >= parsed.value().remaining.size())
        {
            std::cerr << "Missing value for " << argument << '\n'; return 2;
        }
        if (argument == "--url") url = parsed.value().remaining[++i];
        else if (argument == "--ip") address = parsed.value().remaining[++i];
        else if (argument == "--profile") profile = parsed.value().remaining[++i];
        else if (argument == "--config") config_path = parsed.value().remaining[++i];
        else if (argument == "--device") selector = parsed.value().remaining[++i];
        else if (argument == "--diagnose-input") diagnostic = true;
        else if (argument == "--list-inputs") list = true;
        else if (argument == "--raw-joystick") raw = true;
        else if (argument == "--calibrate") calibrate = diagnostic = true;
        else if (argument == "--confirm-movement") confirmed = true;
        else { std::cerr << "Unknown argument: " << argument << '\n'; return 2; }
    }
    InputConfig input_config;
    if (!config_path.empty())
    {
        auto contents = read_file(config_path);
        if (contents.empty()) { std::cerr << "Unable to read configuration: " << config_path << '\n'; return 2; }
        auto loaded = parse_config(contents);
        if (!loaded) { std::cerr << loaded.error().message << '\n'; return 2; }
        input_config = loaded.value();
    }
    if (!diagnostic && !list && (profile.empty() || (url.empty() == address.empty())))
    {
        usage(argv[0]); return 2;
    }
    if (!diagnostic && !list && !confirmed)
    {
        std::cerr << "Camera control requires --confirm-movement\n"; return 2;
    }

    SdlInput input(input_config, selector, raw);
    auto opened = input.open(!list);
    if (!opened) { std::cerr << opened.error().message << '\n'; return 1; }
    if (list)
    {
        const auto devices = input.devices();
        if (devices.empty()) std::cout << "No joystick or gamepad devices found.\n";
        else for (const auto& device : devices) std::cout << device << '\n';
        return 0;
    }
    std::cout << "Input: " << input.active_name() << '\n';
    if (calibrate)
        std::cout << "Calibration view: release controls to observe drift, then exercise every axis. "
                     "Adjust dead_zone/inversion/axis names in the JSON configuration.\n";

    InputModel model(input_config);
    std::shared_ptr<Runtime> runtime;
    std::shared_ptr<PtzController> controller;
    if (!diagnostic)
    {
        auto credentials = wlonvif::examples::load_credentials(common);
        if (!credentials) { std::cerr << credentials.error().message << '\n'; return 2; }
        auto made_runtime = Runtime::create();
        if (!made_runtime) { std::cerr << made_runtime.error().message << '\n'; return 1; }
        runtime = made_runtime.value();
        auto endpoint = wlonvif::examples::resolve_device_endpoint(
            url, address, runtime, common.timeout_seconds);
        if (!endpoint) { std::cerr << endpoint.error().message << '\n'; return 1; }
        auto session = wlonvif::examples::connect_device(
            endpoint.value().url, runtime, credentials.value(), common.timeout_seconds);
        if (!session) { std::cerr << session.error().message << '\n'; return 1; }
        auto ptz = session.value()->ptz();
        if (!ptz) { std::cerr << ptz.error().message << '\n'; return 1; }
        auto created = PtzController::create(ptz.value(), profile);
        if (!created) { std::cerr << created.error().message << '\n'; return 1; }
        controller = created.value();
        if (auto claim = controller->claim_manual("sdl-native"); !claim)
        {
            std::cerr << claim.error().message << '\n'; return 1;
        }
    }

    std::uint64_t sequence = 0;
    auto next_display = std::chrono::steady_clock::now();
    auto stop_visible_until = std::chrono::steady_clock::time_point{};
    std::array<double, 3> calibration_min{1, 1, 1};
    std::array<double, 3> calibration_max{-1, -1, -1};
    bool running = true;
    while (running && !wlonvif::examples::shutdown_requested())
    {
        running = input.poll(model);
        const auto intent = model.intent();
        const bool stop_triggered = model.consume_stop();
        if (stop_triggered) stop_visible_until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        if (controller && stop_triggered)
        {
            auto stopped = wlonvif::examples::wait_for_task(
                controller->stop(wlonvif::examples::safety_call_options(common.timeout_seconds)));
            if (!stopped) std::cerr << "Stop failed: " << stopped.error().message << '\n';
        }
        if (controller)
        {
            auto accepted = controller->set_manual_input("sdl-native", ++sequence, intent);
            if (!accepted) { std::cerr << accepted.error().message << '\n'; running = false; }
        }
        const auto now = std::chrono::steady_clock::now();
        const auto raw_axes = input.raw_axes();
        const std::array<double, 3> raw{
            part(raw_axes.pan), part(raw_axes.tilt), part(raw_axes.zoom)};
        if (calibrate)
        {
            for (std::size_t axis = 0; axis < raw.size(); ++axis)
            {
                calibration_min[axis] = std::min(calibration_min[axis], raw[axis]);
                calibration_max[axis] = std::max(calibration_max[axis], raw[axis]);
            }
        }
        UiFrame frame;
        frame.intent = intent;
        if (controller) frame.controller = controller->state();
        frame.diagnostic = diagnostic;
        frame.calibrating = calibrate;
        frame.precision = model.precision();
        frame.stop_triggered = now < stop_visible_until;
        frame.calibration_min = calibration_min;
        frame.calibration_max = calibration_max;
        input.render(model, frame);
        if (now >= next_display)
        {
            std::cout << "\rpan " << part(intent.pan) << "  tilt " << part(intent.tilt)
                      << "  zoom " << part(intent.zoom);
            if (controller)
            {
                const auto& state = *frame.controller;
                std::cout << "  commands " << state.commands_sent << "  stops " << state.stops_sent
                          << "  request-us " << state.last_request_latency.count();
            }
            std::cout << "        " << std::flush;
            next_display = now + std::chrono::milliseconds(200);
        }
        SDL_Delay(20);
    }
    std::cout << '\n';
    if (controller)
    {
        (void)controller->release_manual("sdl-native");
        auto closed = wlonvif::examples::wait_for_task(
            controller->close(wlonvif::examples::safety_call_options(common.timeout_seconds)));
        if (!closed) { std::cerr << "Final Stop failed: " << closed.error().message << '\n'; return 1; }
    }
    if (runtime) (void)runtime->shutdown();
}

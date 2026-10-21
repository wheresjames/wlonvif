#include <cmath>
#include <future>
#include <iostream>
#include <thread>
#include <wlonvif/wlonvif.hpp>

using namespace wlonvif;

namespace
{
    template <class T> T wait(Task<T> task)
    {
        std::promise<T> promise;
        auto future = promise.get_future();
        std::move(task).start([&](T value) { promise.set_value(std::move(value)); });
        return future.get();
    }

    CallOptions options()
    {
        return {std::chrono::steady_clock::now() + std::chrono::seconds(3),
                CancellationSource{}.token()};
    }
}

#define CHECK(expression)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (!(expression))                                                                         \
        {                                                                                          \
            std::cerr << "check failed: " #expression " at line " << __LINE__ << '\n';          \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main()
{
    PtzControllerConfig config;
    config.dead_zone = 0;
    config.response_exponent = 1;
    config.maximum_pan = config.maximum_tilt = config.maximum_zoom = 1;
    config.minimum_effective_speed = 0;
    config.ramp_up = std::chrono::milliseconds(1000);
    config.ramp_down = std::chrono::milliseconds(500);
    config.zoomed_pan_tilt_scale = 0.2;
    PtzControlLaw law(config);
    CHECK(law.set_input({1.0, 1.0, 0.0}));
    auto first = law.advance(std::chrono::milliseconds(100), -1.0);
    CHECK(std::abs(first.pan.value_or(0) - 0.1) < 1e-9);
    CHECK(std::abs(first.tilt.value_or(0) - 0.1) < 1e-9);
    auto zoomed = law.advance(std::chrono::milliseconds(100), 1.0);
    CHECK(zoomed.pan.value_or(0) < 0.15);
    CHECK(law.desired().pan.value_or(0) < 0.15);
    law.release();
    auto released = law.advance(std::chrono::milliseconds(100), 1.0);
    CHECK(std::abs(released.pan.value_or(0)) < 1e-9);
    CHECK(!law.set_input({1.1, {}, {}}));

    PtzControllerConfig tuned = config;
    tuned.ramp_mode = PtzRampMode::firmware;
    tuned.pan_limits = NativeRange{-0.5, 0.5};
    tuned.zoom_speed_curve = {{-1, 1}, {0, 0.5}, {1, 0.1}};
    PtzControlLaw tuned_law(tuned);
    CHECK(tuned_law.set_input({1.0, 0.0, 0.0}));
    auto limited = tuned_law.advance(std::chrono::milliseconds(1), PtzVector{0.5, 0.0, 0.0});
    CHECK(std::abs(limited.pan.value_or(1)) < 1e-9);
    tuned_law.reset();
    CHECK(tuned_law.set_input({-1.0, 0.0, 0.0}));
    auto calibrated = tuned_law.advance(std::chrono::milliseconds(1), PtzVector{0.0, 0.0, 0.0});
    CHECK(std::abs(calibrated.pan.value_or(0) + 0.5) < 1e-9);

    tuned.keep_out_zones = {{-0.1, 0.1, -0.2, 0.2}};
    PtzControlLaw keep_out(tuned);
    CHECK(keep_out.set_input({1.0, 0.0, 0.0}));
    auto blocked = keep_out.advance(
        std::chrono::milliseconds(1), PtzVector{-0.1, 0.0, -1.0});
    CHECK(std::abs(blocked.pan.value_or(1)) < 1e-9);

    auto runtime_result = Runtime::create();
    CHECK(runtime_result);
    auto runtime = runtime_result.value();
    auto device = std::make_shared<VirtualDeviceTransport>(
        runtime, DeviceInformation{}, std::vector<std::string>{}, std::vector<Service>{},
        Capabilities{}
    );
    device->add_profile(
        {"main", "Main", {}}, MediaUri{"rtsp://camera/main", {}, false},
        MediaUri{"http://camera/snapshot", {}, false}, PtzSpaces{{-1, 1}, {-1, 1}, {-1, 1}}
    );
    auto client = std::make_shared<PtzClient>(runtime, device);
    PtzControllerConfig live;
    live.tick_interval = std::chrono::milliseconds(2);
    live.command_interval = std::chrono::milliseconds(5);
    live.status_interval = std::chrono::milliseconds(20);
    live.ramp_up = live.ramp_down = std::chrono::milliseconds(0);
    live.input_watchdog = std::chrono::milliseconds(30);
    live.maximum_run = std::chrono::milliseconds(200);
    auto created = PtzController::create(client, "main", live);
    CHECK(created);
    auto controller = created.value();
    CHECK(controller->claim_manual("test"));
    CHECK(controller->set_manual_input("test", 1, {0.5, 0.25, {}}));
    CHECK(!controller->set_manual_input("test", 1, {0.2, {}, {}}));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(controller->state().commands_sent >= 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(controller->state().stops_sent >= 1);
    CHECK(controller->relinquish_manual("test"));
    CHECK(controller->claim_manual("second"));
    CHECK(controller->set_manual_input("second", 1, {0.1, {}, {}}));
    CHECK(controller->release_manual("second"));
    CHECK(wait(controller->close(options())));
    CHECK(controller->state().mode == PtzControllerMode::closed);
    CHECK(runtime->shutdown());
    std::cout << "PTZ controller tests passed\n";
}

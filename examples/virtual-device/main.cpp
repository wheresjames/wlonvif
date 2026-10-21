/**
 * @file main.cpp
 * @brief wlonvif-virtual-device: the library's simulated camera, used like a real one.
 *
 * VirtualDeviceTransport answers ONVIF requests in memory, so an application
 * (or its tests) can run the same session, media, PTZ, and event code without
 * a camera or a network. This example builds one camera, connects to it, and
 * reads back what it was given.
 */

#include <example_support.hpp>
#include <iostream>

int main()
{
    auto runtime_result = wlonvif::Runtime::create();
    if (!runtime_result)
    {
        std::cerr << runtime_result.error().message << '\n';
        return 1;
    }
    auto runtime = runtime_result.value();
    // The simulated camera: its identity, scopes, advertised services, and capabilities.
    auto transport = std::make_shared<wlonvif::VirtualDeviceTransport>(
        runtime,
        wlonvif::DeviceInformation{"Example", "Virtual Camera", "1.0", "VIRTUAL-1", "sim"},
        std::vector<std::string>{"onvif://www.onvif.org/name/VirtualCamera"},
        std::vector<wlonvif::Service>{},
        wlonvif::Capabilities{}
    );
    // One media profile with stream and snapshot URIs, and pan, tilt, and zoom ranges.
    transport->add_profile(
        {"main", "Main profile", {}},
        {"rtsp://virtual.example/main", std::chrono::seconds(60), false},
        {"http://virtual.example/snapshot.jpg", std::chrono::seconds(60), false},
        {{-1, 1}, {-1, 1}, {0, 1}}
    );
    transport->add_preset("main", {"home", "Home", {{0.0}, {0.0}, {0.0}}});
    // An event waiting in the camera's queue, for the event stream to deliver.
    transport->emit(
        {"tns1:VideoSource/MotionAlarm",
         std::chrono::system_clock::now(),
         "VideoSource_1",
         "true",
         1}
    );
    // The transport stands in for HTTP: the URL is never resolved or contacted.
    auto connected = wlonvif::examples::wait_for_task(
        wlonvif::DeviceSession::connect(
            "http://virtual.example/onvif/device_service",
            transport,
            wlonvif::make_same_origin_policy(),
            runtime,
            std::nullopt,
            wlonvif::examples::call_options(5)
        )
    );
    if (!connected)
    {
        std::cerr << connected.error().message << '\n';
        return 1;
    }
    auto information = wlonvif::examples::wait_for_task(
        connected.value()->get_device_information(wlonvif::examples::call_options(5))
    );
    auto media = connected.value()->media();
    auto ptz = connected.value()->ptz();
    auto events = connected.value()->events();
    if (!information || !media || !ptz || !events)
    {
        std::cerr << "Virtual service setup failed\n";
        return 1;
    }
    // The same calls an application makes against a real camera.
    auto profiles = wlonvif::examples::wait_for_task(
        media.value()->get_profiles(wlonvif::examples::call_options(5))
    );
    auto presets = wlonvif::examples::wait_for_task(
        ptz.value()->get_presets("main", wlonvif::examples::call_options(5))
    );
    auto polled = wlonvif::examples::wait_for_task(
        events.value()->poll(10, wlonvif::examples::call_options(5))
    );
    auto event =
        wlonvif::examples::wait_for_task(events.value()->next(wlonvif::examples::call_options(5)));
    if (!profiles || !presets || !polled || !event)
    {
        std::cerr << "Virtual workflow failed\n";
        return 1;
    }
    std::cout << information.value().manufacturer << ' ' << information.value().model << '\n'
              << "Profiles: " << profiles.value().size() << '\n'
              << "Presets: " << presets.value().size() << '\n'
              << "Event received: "
              << (std::holds_alternative<wlonvif::Event>(event.value()) ? "yes" : "health signal")
              << '\n';
    auto closed =
        wlonvif::examples::wait_for_task(events.value()->close(wlonvif::examples::call_options(5)));
    if (!closed)
    {
        std::cerr << closed.error().message << '\n';
        return 1;
    }
    (void)runtime->shutdown();
    return 0;
}

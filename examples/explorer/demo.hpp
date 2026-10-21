#pragma once

/**
 * @file demo.hpp
 * @brief A simulated camera, so the explorer can be tried without hardware.
 */

#include <memory>
#include <wlonvif/wlonvif.hpp>

namespace wlonvif::examples::explorer
{
    /// Device service URL the explorer uses for the simulated camera.
    inline constexpr const char* demo_url = "http://demo.invalid/onvif/device_service";

    /// A virtual camera with profiles, PTZ, imaging, analytics, and event topics.
    std::shared_ptr<VirtualDeviceTransport>
    make_demo_camera(const std::shared_ptr<Runtime>& runtime);
} // namespace wlonvif::examples::explorer

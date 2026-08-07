#pragma once
#include <string_view>
#include <wlonvif/wlonvif.hpp>
namespace wlonvif::examples::ptz_web
{
    Result<ViewportCalibration> parse_viewport_calibration(std::string_view json);
}

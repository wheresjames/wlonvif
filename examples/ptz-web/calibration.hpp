#pragma once

/**
 * @file calibration.hpp
 * @brief Loading the viewport calibration from --viewport-calibration.
 *
 * Click-to-centre and box zoom turn a position in the browser's view into a
 * relative camera move. How far the camera turns for a given fraction of the
 * view depends on the lens and the zoom, so a calibration file can describe it:
 * where the picture sits in the view, rotation and mirroring, gains, and
 * per-zoom gains for each direction. Without one, defaults are used.
 */

#include <string_view>
#include <wlonvif/wlonvif.hpp>

namespace wlonvif::examples::ptz_web
{
    /// Parse a version 1 calibration; unknown fields and out-of-range values are refused.
    Result<ViewportCalibration> parse_viewport_calibration(std::string_view json);
} // namespace wlonvif::examples::ptz_web

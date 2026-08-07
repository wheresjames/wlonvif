#include <wlonvif/wlonvif.hpp>
#include <cmath>
#include <iostream>
using namespace wlonvif;
#define CHECK(x) do { if (!(x)) { std::cerr << "check failed: " #x << " line " << __LINE__ << '\n'; return 1; } } while (0)
int main()
{
    ViewportCalibration calibration;
    calibration.calibrated = true;
    ViewportMapper mapper(calibration);
    auto center = mapper.click_to_center({320, 180, 640, 360});
    CHECK(center && center.value().calibrated);
    CHECK(std::abs(center.value().translation.pan.value_or(1)) < 1e-9);
    auto corner = mapper.click_to_center({640, 0, 640, 360});
    CHECK(corner && corner.value().translation.pan.value_or(0) > 0);
    CHECK(corner.value().translation.tilt.value_or(0) > 0);
    calibration.content_y = 0.125;
    calibration.content_height = 0.75;
    ViewportMapper letterbox(calibration);
    CHECK(!letterbox.click_to_center({10, 10, 640, 480}));
    calibration.content_y = 0;
    calibration.content_height = 1;
    calibration.rotation = ViewportRotation::degrees_90;
    calibration.mirror_horizontal = true;
    auto transformed = ViewportMapper(calibration).click_to_center({640, 180, 640, 360});
    CHECK(transformed);
    auto box = mapper.box_zoom({160, 90, 640, 360}, {480, 270, 640, 360});
    CHECK(box && box.value().translation.zoom.value_or(0) > 0);
    auto stale = mapper.click_to_center({640, 180, 640, 360}, std::chrono::hours(25));
    CHECK(stale && !stale.value().calibrated);
    ViewportCalibration directional;
    directional.calibrated = true;
    directional.maximum_translation = 1;
    directional.zoom_points = {
        {-1, 0.1, 0.2, 0.3, 0.4},
        {1, 0.5, 0.6, 0.7, 0.8}
    };
    ViewportMapper directional_mapper(directional);
    auto right_mid_zoom = directional_mapper.click_to_center(
        {75, 50, 100, 100}, {}, 0.0);
    CHECK(right_mid_zoom &&
          std::abs(right_mid_zoom.value().translation.pan.value_or(0) - 0.2) < 1e-9);
    auto left_wide = directional_mapper.click_to_center(
        {25, 50, 100, 100}, {}, -1.0);
    CHECK(left_wide &&
          std::abs(left_wide.value().translation.pan.value_or(0) + 0.05) < 1e-9);
    directional.zoom_points = {{0, 0.3, 0.4, 0.5, 0.6}};
    auto single_without_status = ViewportMapper(directional).click_to_center({75, 50, 100, 100});
    CHECK(single_without_status &&
          std::abs(single_without_status.value().translation.pan.value_or(0) - 0.2) < 1e-9);
    directional.zoom_points = {{0.5, 1, 1, 1, 1}, {0.4, 1, 1, 1, 1}};
    CHECK(!ViewportMapper(directional).click_to_center({50, 50, 100, 100}));
    PtzSettlingDetector settling(std::chrono::milliseconds(200), std::chrono::seconds(1), 0.01);
    PtzStatus status{{0.0, 0.0, 0.0}, false, false, false, true};
    CHECK(settling.update(std::chrono::milliseconds(100), status) == PtzArrival::moving);
    CHECK(settling.update(std::chrono::milliseconds(100), status) == PtzArrival::moving);
    CHECK(settling.update(std::chrono::milliseconds(100), status) == PtzArrival::settled);
    status.position_reliable = false;
    CHECK(settling.update(std::chrono::milliseconds(1), status) == PtzArrival::degraded);
    std::cout << "PTZ viewport tests passed\n";
}

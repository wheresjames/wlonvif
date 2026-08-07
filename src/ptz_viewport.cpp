#include <wlonvif/wlonvif.hpp>

#include <algorithm>
#include <cmath>

namespace wlonvif
{
    namespace
    {
        Result<void> validate(const ViewportCalibration& value)
        {
            const auto finite = [](double number) { return std::isfinite(number); };
            if (!finite(value.content_x) || !finite(value.content_y) ||
                !finite(value.content_width) || !finite(value.content_height) ||
                value.content_x < 0 || value.content_y < 0 || value.content_width <= 0 ||
                value.content_height <= 0 || value.content_x + value.content_width > 1 ||
                value.content_y + value.content_height > 1 || !finite(value.pan_gain) ||
                !finite(value.tilt_gain) || !finite(value.box_zoom_gain) ||
                !finite(value.maximum_translation) || value.pan_gain < 0 ||
                value.tilt_gain < 0 || value.box_zoom_gain < 0 ||
                value.maximum_translation <= 0 || value.maximum_translation > 1 ||
                value.maximum_age.count() < 0)
                return Error{ErrorCategory::invalid_argument, OperationId::relative_move,
                             "invalid viewport calibration"};
            double previous_zoom = -2;
            for (const auto& point : value.zoom_points)
            {
                if (!finite(point.zoom) || point.zoom < -1 || point.zoom > 1 ||
                    point.zoom <= previous_zoom || !finite(point.pan_left_gain) ||
                    !finite(point.pan_right_gain) || !finite(point.tilt_up_gain) ||
                    !finite(point.tilt_down_gain) || point.pan_left_gain < 0 ||
                    point.pan_right_gain < 0 || point.tilt_up_gain < 0 ||
                    point.tilt_down_gain < 0)
                    return Error{ErrorCategory::invalid_argument, OperationId::relative_move,
                                 "invalid viewport zoom calibration points"};
                previous_zoom = point.zoom;
            }
            return {};
        }

        ViewportCalibrationPoint gains(const ViewportCalibration& calibration,
                                       std::optional<double> zoom)
        {
            ViewportCalibrationPoint fallback{0, calibration.pan_gain, calibration.pan_gain,
                                               calibration.tilt_gain, calibration.tilt_gain};
            if (calibration.zoom_points.empty()) return fallback;
            if (calibration.zoom_points.size() == 1) return calibration.zoom_points.front();
            if (!zoom || !std::isfinite(*zoom)) return fallback;
            const double value = std::clamp(*zoom, -1.0, 1.0);
            if (value <= calibration.zoom_points.front().zoom)
                return calibration.zoom_points.front();
            if (value >= calibration.zoom_points.back().zoom)
                return calibration.zoom_points.back();
            const auto upper = std::upper_bound(calibration.zoom_points.begin(),
                calibration.zoom_points.end(), value,
                [](double candidate, const auto& point) { return candidate < point.zoom; });
            const auto& high = *upper;
            const auto& low = *(upper - 1);
            const double fraction = (value - low.zoom) / (high.zoom - low.zoom);
            const auto mix = [fraction](double a, double b) { return a + (b - a) * fraction; };
            return {value, mix(low.pan_left_gain, high.pan_left_gain),
                    mix(low.pan_right_gain, high.pan_right_gain),
                    mix(low.tilt_up_gain, high.tilt_up_gain),
                    mix(low.tilt_down_gain, high.tilt_down_gain)};
        }

        Result<std::pair<double, double>> coordinates(
            const ViewportCalibration& calibration, const ViewportPoint& point
        )
        {
            if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
                !std::isfinite(point.width) || !std::isfinite(point.height) ||
                point.width <= 0 || point.height <= 0)
                return Error{ErrorCategory::invalid_argument, OperationId::relative_move,
                             "invalid viewport point"};
            const double left = calibration.content_x * point.width;
            const double top = calibration.content_y * point.height;
            const double width = calibration.content_width * point.width;
            const double height = calibration.content_height * point.height;
            if (point.x < left || point.x > left + width || point.y < top || point.y > top + height)
                return Error{ErrorCategory::invalid_argument, OperationId::relative_move,
                             "point is outside displayed video content"};
            double x = 2 * ((point.x - left) / width) - 1;
            double y = 1 - 2 * ((point.y - top) / height);
            if (calibration.mirror_horizontal) x = -x;
            if (calibration.mirror_vertical) y = -y;
            switch (calibration.rotation)
            {
            case ViewportRotation::degrees_0: break;
            case ViewportRotation::degrees_90: std::tie(x, y) = std::pair{y, -x}; break;
            case ViewportRotation::degrees_180: x = -x; y = -y; break;
            case ViewportRotation::degrees_270: std::tie(x, y) = std::pair{-y, x}; break;
            }
            return std::pair{x, y};
        }
    }

    ViewportMapper::ViewportMapper(ViewportCalibration calibration)
        : calibration_(std::move(calibration)) {}

    Result<ViewportMove> ViewportMapper::click_to_center(
        ViewportPoint point, std::chrono::milliseconds calibration_age,
        std::optional<double> normalized_zoom
    ) const
    {
        if (auto valid = validate(calibration_); !valid) return valid.error();
        if (calibration_age.count() < 0)
            return Error{ErrorCategory::invalid_argument, OperationId::relative_move,
                         "calibration age cannot be negative"};
        auto mapped = coordinates(calibration_, point);
        if (!mapped) return mapped.error();
        const bool calibrated = calibration_.calibrated && calibration_age <= calibration_.maximum_age;
        const double safety = calibrated ? 1.0 : 0.25;
        const double limit = calibrated ? calibration_.maximum_translation
                                        : std::min(0.2, calibration_.maximum_translation);
        const auto directional = gains(calibration_, normalized_zoom);
        const double pan_gain = mapped.value().first < 0 ? directional.pan_left_gain
                                                         : directional.pan_right_gain;
        const double tilt_gain = mapped.value().second < 0 ? directional.tilt_down_gain
                                                            : directional.tilt_up_gain;
        return ViewportMove{{
            std::clamp(mapped.value().first * pan_gain * safety, -limit, limit),
            std::clamp(mapped.value().second * tilt_gain * safety, -limit, limit),
            {}
        }, calibrated};
    }

    Result<ViewportMove> ViewportMapper::box_zoom(
        ViewportPoint first, ViewportPoint second, std::chrono::milliseconds calibration_age,
        std::optional<double> normalized_zoom
    ) const
    {
        if (first.width != second.width || first.height != second.height)
            return Error{ErrorCategory::invalid_argument, OperationId::relative_move,
                         "box points must use the same viewport"};
        ViewportPoint center{(first.x + second.x) / 2, (first.y + second.y) / 2,
                             first.width, first.height};
        auto movement = click_to_center(center, calibration_age, normalized_zoom);
        if (!movement) return movement.error();
        const double width_fraction = std::abs(first.x - second.x) /
            (calibration_.content_width * first.width);
        const double height_fraction = std::abs(first.y - second.y) /
            (calibration_.content_height * first.height);
        if (width_fraction <= 0 || height_fraction <= 0 || width_fraction > 1 || height_fraction > 1)
            return Error{ErrorCategory::invalid_argument, OperationId::relative_move,
                         "invalid box zoom region"};
        const double safety = movement.value().calibrated ? 1.0 : 0.25;
        movement.value().translation.zoom = std::clamp(
            (1 - std::max(width_fraction, height_fraction)) * calibration_.box_zoom_gain * safety,
            0.0, calibration_.maximum_translation);
        return movement;
    }

    PtzSettlingDetector::PtzSettlingDetector(
        std::chrono::milliseconds stable_duration, std::chrono::milliseconds timeout,
        double position_epsilon
    ) : stable_duration_(stable_duration), timeout_(timeout), epsilon_(position_epsilon) {}

    PtzArrival PtzSettlingDetector::update(std::chrono::milliseconds elapsed, const PtzStatus& status)
    {
        if (elapsed.count() < 0 || stable_duration_.count() < 0 || timeout_.count() <= 0 ||
            !std::isfinite(epsilon_) || epsilon_ < 0) return PtzArrival::degraded;
        total_ += elapsed;
        if (!status.position_reliable) return PtzArrival::degraded;
        const auto difference = [](const std::optional<double>& a, const std::optional<double>& b)
        {
            return a && b ? std::abs(*a - *b) : 0.0;
        };
        const bool stationary = !status.moving && previous_ &&
            difference(status.position.pan, previous_->pan) <= epsilon_ &&
            difference(status.position.tilt, previous_->tilt) <= epsilon_ &&
            difference(status.position.zoom, previous_->zoom) <= epsilon_;
        stable_ = stationary ? stable_ + elapsed : std::chrono::milliseconds{0};
        previous_ = status.position;
        if (stable_ >= stable_duration_) return PtzArrival::settled;
        return total_ >= timeout_ ? PtzArrival::timed_out : PtzArrival::moving;
    }

    void PtzSettlingDetector::reset() noexcept
    {
        total_ = stable_ = std::chrono::milliseconds{0};
        previous_.reset();
    }
}

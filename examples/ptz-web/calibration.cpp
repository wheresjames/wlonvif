#include "calibration.hpp"
#include <boost/json.hpp>
#include <algorithm>
#include <set>
namespace wlonvif::examples::ptz_web
{
    Result<ViewportCalibration> parse_viewport_calibration(std::string_view input)
    {
        boost::system::error_code error;
        auto value = boost::json::parse(input, error);
        if (error || !value.is_object())
            return Error{ErrorCategory::invalid_argument, OperationId::relative_move,
                         "malformed viewport calibration JSON"};
        const auto& object = value.as_object();
        const std::set<std::string_view> allowed{"version", "calibrated", "content_x",
            "content_y", "content_width", "content_height", "rotation", "mirror_horizontal",
            "mirror_vertical", "pan_gain", "tilt_gain", "box_zoom_gain",
            "maximum_translation", "maximum_age_ms", "zoom_points"};
        for (const auto& field : object)
            if (!allowed.contains(field.key()))
                return Error{ErrorCategory::invalid_argument, OperationId::relative_move,
                             "unknown viewport calibration field"};
        const auto integer = [&](std::string_view name) -> std::optional<std::int64_t>
        {
            const auto* field = object.if_contains(name);
            return field && field->is_int64() ? std::optional{field->as_int64()} : std::nullopt;
        };
        const auto number = [&](std::string_view name, double fallback) -> std::optional<double>
        {
            const auto* field = object.if_contains(name);
            if (!field) return fallback;
            return field->is_number() ? std::optional{field->to_number<double>()} : std::nullopt;
        };
        const auto boolean = [&](std::string_view name, bool fallback) -> std::optional<bool>
        {
            const auto* field = object.if_contains(name);
            if (!field) return fallback;
            return field->is_bool() ? std::optional{field->as_bool()} : std::nullopt;
        };
        if (integer("version") != 1)
            return Error{ErrorCategory::invalid_argument, OperationId::relative_move,
                         "viewport calibration requires version 1"};
        ViewportCalibration result;
        const auto calibrated = boolean("calibrated", true);
        const auto content_x = number("content_x", result.content_x);
        const auto content_y = number("content_y", result.content_y);
        const auto content_width = number("content_width", result.content_width);
        const auto content_height = number("content_height", result.content_height);
        const auto rotation = integer("rotation");
        const auto mirror_horizontal = boolean("mirror_horizontal", false);
        const auto mirror_vertical = boolean("mirror_vertical", false);
        const auto pan_gain = number("pan_gain", result.pan_gain);
        const auto tilt_gain = number("tilt_gain", result.tilt_gain);
        const auto box_zoom_gain = number("box_zoom_gain", result.box_zoom_gain);
        const auto maximum_translation = number("maximum_translation", result.maximum_translation);
        const auto maximum_age = integer("maximum_age_ms");
        if (!calibrated || !content_x || !content_y || !content_width || !content_height ||
            !rotation || !mirror_horizontal || !mirror_vertical || !pan_gain || !tilt_gain ||
            !box_zoom_gain || !maximum_translation || !maximum_age || *maximum_age < 0)
            return Error{ErrorCategory::invalid_argument, OperationId::relative_move,
                         "invalid viewport calibration field type"};
        if (*rotation == 0) result.rotation = ViewportRotation::degrees_0;
        else if (*rotation == 90) result.rotation = ViewportRotation::degrees_90;
        else if (*rotation == 180) result.rotation = ViewportRotation::degrees_180;
        else if (*rotation == 270) result.rotation = ViewportRotation::degrees_270;
        else return Error{ErrorCategory::invalid_argument, OperationId::relative_move,
                          "viewport rotation must be 0, 90, 180, or 270"};
        result.calibrated = *calibrated;
        result.content_x = *content_x; result.content_y = *content_y;
        result.content_width = *content_width; result.content_height = *content_height;
        result.mirror_horizontal = *mirror_horizontal; result.mirror_vertical = *mirror_vertical;
        result.pan_gain = *pan_gain; result.tilt_gain = *tilt_gain;
        result.box_zoom_gain = *box_zoom_gain; result.maximum_translation = *maximum_translation;
        result.maximum_age = std::chrono::milliseconds(*maximum_age);
        if (const auto* points = object.if_contains("zoom_points"))
        {
            if (!points->is_array() || points->as_array().size() > 32)
                return Error{ErrorCategory::invalid_argument, OperationId::relative_move,
                             "viewport zoom_points must be an array with at most 32 entries"};
            const std::set<std::string_view> point_fields{"zoom", "pan_left_gain",
                "pan_right_gain", "tilt_up_gain", "tilt_down_gain"};
            for (const auto& value : points->as_array())
            {
                if (!value.is_object())
                    return Error{ErrorCategory::invalid_argument, OperationId::relative_move,
                                 "viewport zoom calibration point must be an object"};
                const auto& point = value.as_object();
                for (const auto& field : point)
                    if (!point_fields.contains(field.key()))
                        return Error{ErrorCategory::invalid_argument, OperationId::relative_move,
                                     "unknown viewport zoom calibration field"};
                const auto required = [&](std::string_view name) -> std::optional<double>
                {
                    const auto* field = point.if_contains(name);
                    return field && field->is_number()
                        ? std::optional{field->to_number<double>()} : std::nullopt;
                };
                const auto zoom = required("zoom");
                const auto left = required("pan_left_gain");
                const auto right = required("pan_right_gain");
                const auto up = required("tilt_up_gain");
                const auto down = required("tilt_down_gain");
                if (!zoom || !left || !right || !up || !down)
                    return Error{ErrorCategory::invalid_argument, OperationId::relative_move,
                                 "incomplete viewport zoom calibration point"};
                result.zoom_points.push_back({*zoom, *left, *right, *up, *down});
            }
        }
        // Constructing a mapper and mapping the center validates all range relationships.
        auto validation = ViewportMapper(result).click_to_center({50, 50, 100, 100});
        if (!validation) return validation.error();
        return result;
    }
}

/**
 * @file protocol_tests.cpp
 * @brief Tests of the control protocol: sessions, leases, sequence numbers, limits, and every
 * message type, plus viewport calibration parsing.
 */

#include "calibration.hpp"
#include "protocol.hpp"

#include <iostream>
using namespace wlonvif;
using namespace wlonvif::examples::ptz_web;
#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(x))                                                                                  \
        {                                                                                          \
            std::cerr << "check failed: " #x << " line " << __LINE__ << '\n';                      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
int main()
{
    ProtocolConfig config{{"http://127.0.0.1:8080"}, std::chrono::milliseconds(100), 256, 4};
    ControlProtocol protocol(config);
    const auto now = std::chrono::steady_clock::time_point{};
    const std::string session(32, 'a');
    CHECK(!protocol.create_session("http://evil", now, session));
    CHECK(protocol.create_session(config.allowed_origins[0], now, session));
    auto input = protocol.handle(
        session,
        config.allowed_origins[0],
        R"({"version":1,"type":"intent","sequence":1,"pan":0.5,"tilt":0,"zoom":-0.2})",
        now
    );
    CHECK(input && input.value().type == CommandType::intent);
    CHECK(!protocol.handle(
        session, config.allowed_origins[0], R"({"version":1,"type":"stop","sequence":1})", now
    ));
    auto click = protocol.handle(
        session,
        config.allowed_origins[0],
        R"({"version":1,"type":"click","sequence":2,"x":50,"y":50,"width":100,"height":100,"invert_pan":true,"invert_tilt":false})",
        now
    );
    CHECK(click && click.value().invert_pan && !click.value().invert_tilt);
    CHECK(protocol.handle(
        session, config.allowed_origins[0], R"({"version":1,"type":"stop","sequence":3})", now
    ));
    CHECK(!protocol.handle(
        session, config.allowed_origins[0], R"({"version":1,"type":"stop","sequence":4})", now
    )); // rate limit
    CHECK(
        !protocol.handle(session, "http://evil", R"({"version":1,"type":"stop","sequence":5})", now)
    );
    CHECK(!protocol.handle(session, config.allowed_origins[0], std::string(300, 'x'), now));
    CHECK(!protocol.handle(
        session,
        config.allowed_origins[0],
        R"({"version":1,"type":"stop","type":"intent","sequence":6})",
        now
    ));
    CHECK(!protocol.handle(
        session, config.allowed_origins[0], R"({"version":"1","type":"stop","sequence":6})", now
    ));
    CHECK(!protocol.handle(
        session,
        config.allowed_origins[0],
        R"({"version":1,"type":"stop","sequence":6})",
        now + std::chrono::milliseconds(101)
    ));
    CHECK(protocol.expire(now + std::chrono::milliseconds(101)).empty());
    CHECK(protocol.create_session(config.allowed_origins[0], now, session));
    CHECK(protocol.disconnect(session));
    CHECK(!protocol.disconnect(session));
    auto calibration = parse_viewport_calibration(R"({"version":1,"calibrated":true,
      "content_x":0,"content_y":0.1,"content_width":1,"content_height":0.8,
      "rotation":90,"mirror_horizontal":false,"mirror_vertical":true,
      "pan_gain":0.4,"tilt_gain":0.3,"box_zoom_gain":0.7,
      "maximum_translation":0.4,"maximum_age_ms":60000})");
    CHECK(calibration && calibration.value().rotation == ViewportRotation::degrees_90);
    auto zoom_calibration = parse_viewport_calibration(
        R"({"version":1,"calibrated":true,
      "rotation":0,"maximum_age_ms":60000,"zoom_points":[
      {"zoom":-1,"pan_left_gain":0.2,"pan_right_gain":0.3,"tilt_up_gain":0.4,"tilt_down_gain":0.5},
      {"zoom":1,"pan_left_gain":0.6,"pan_right_gain":0.7,"tilt_up_gain":0.8,"tilt_down_gain":0.9}]})"
    );
    CHECK(zoom_calibration && zoom_calibration.value().zoom_points.size() == 2);
    CHECK(!parse_viewport_calibration(R"({"version":1,"rotation":0,"maximum_age_ms":1,
      "zoom_points":[{"zoom":0,"pan_left_gain":1}]})"));
    CHECK(!parse_viewport_calibration(R"({"version":2})"));
    CHECK(!parse_viewport_calibration(R"({"version":1,"unknown":2})"));
    ProtocolConfig tour_config{{"http://127.0.0.1:8080"}};
    ControlProtocol tours(tour_config);
    CHECK(tours.create_session(tour_config.allowed_origins[0], now, session));
    auto tour = tours.handle(
        session,
        tour_config.allowed_origins[0],
        R"({"version":1,"type":"tour","sequence":1,"presets":["one","two"],"dwell_ms":500,"repeats":2})",
        now
    );
    CHECK(tour && tour.value().tour_presets.size() == 2 && tour.value().repeats == 2);
    CHECK(!tours.handle(
        session,
        tour_config.allowed_origins[0],
        R"({"version":1,"type":"tour","sequence":2,"presets":[],"dwell_ms":10,"repeats":0})",
        now
    ));
    auto selected = tours.handle(
        session,
        tour_config.allowed_origins[0],
        R"({"version":1,"type":"select_profile","sequence":2,"profile":"profile_2"})",
        now
    );
    CHECK(
        selected && selected.value().type == CommandType::select_profile &&
        selected.value().profile == "profile_2"
    );
    std::cout << "PTZ web protocol tests passed\n";
}

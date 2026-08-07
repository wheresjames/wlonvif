#include "input_model.hpp"

#include <cmath>
#include <iostream>

using namespace wlonvif::examples::ptz_control;
#define CHECK(x) do { if (!(x)) { std::cerr << "check failed: " #x << " line " << __LINE__ << '\n'; return 1; } } while (0)

int main()
{
    auto parsed = parse_config(config_json(InputConfig{}));
    CHECK(parsed);
    CHECK(!parse_config("{\"version\":2}"));
    CHECK(!parse_config("{\"version\":1,\"dead_zone\":2}"));
    CHECK(!parse_config("{\"version\":1,\"dead_zone\":\"bad\"}"));
    CHECK(!parse_config("{\"version\":1,\"pan_axis\":\"bad\"}"));
    InputModel input(parsed.value());
    input.key(Key::right, true);
    input.key(Key::right, true); // auto-repeat must not accumulate state
    const auto right = input.intent();
    CHECK(right.pan.value_or(0) > 0);
    for (int i = 0; i < 10000; ++i) input.axes(0.01, -0.01, 0.01);
    CHECK(input.intent().pan.value_or(0) > 0);
    input.key(Key::right, false);
    input.key(Key::left, true);
    input.key(Key::up, true);
    CHECK(input.intent().pan.value_or(0) < 0 && input.intent().tilt.value_or(0) > 0);
    input.key(Key::left, false);
    input.key(Key::up, false);
    input.axes(0.5, -0.5, 0.5);
    input.key(Key::precision, true);
    CHECK(std::abs(input.intent().pan.value_or(0) - 0.125) < 1e-9);
    input.device(true);
    input.device(false);
    CHECK(input.consume_stop());
    input.focus(false);
    CHECK(input.consume_stop());
    CHECK(std::abs(input.intent().pan.value_or(1)) < 1e-9);
    std::cout << "PTZ native input tests passed\n";
}

# Native PTZ control

`wlonvif-ptz-control` is an optional SDL3 frontend for `PtzController`. SDL owns
device and key state; all movement policy remains in the library. The frontend
supports keyboard, SDL-mapped gamepads, raw joysticks, hot-plug, device selection,
focus-loss safety, input diagnostics, and versioned configuration.

The native window is an operator and calibration panel rather than a video viewer.
It renders focus/device state, diagnostic or live-camera status, pan/tilt and zoom
gauges, raw and processed values, dead zone, precision state, calibration ranges,
emergency Stop feedback, controller mode, command counters, and latency telemetry.

## Build and run

```sh
cmake -S . -B build-native \
  -DWLONVIF_BUILD_EXAMPLES=ON \
  -DWLONVIF_BUILD_PTZ_CONTROL=ON
cmake --build build-native --target wlonvif-ptz-control

./build-native/examples/ptz-control/wlonvif-ptz-control --list-inputs
./build-native/examples/ptz-control/wlonvif-ptz-control --diagnose-input
./build-native/examples/ptz-control/wlonvif-ptz-control --calibrate --device "Controller"
```

Use `WLONVIF_FETCH_PTZ_DEPENDENCIES=ON` when SDL3 3.4 is not installed. SDL is
optional and is never linked into the core wlonvif libraries.

Camera operation requires an explicit endpoint, profile, and movement confirmation:

```sh
./build-native/examples/ptz-control/wlonvif-ptz-control \
  --ip 192.168.10.34 --profile profile_1 \
  --username USER --password-file CAMERA_PASSWORD \
  --confirm-movement
```

WASD or arrows control pan/tilt, Q/E controls zoom, Shift and the gamepad left
shoulder select precision mode, Space and gamepad Back request emergency Stop, and
Escape exits with a final Stop. Losing window focus or the selected input device
also requests Stop. The input lease is renewed at 50 Hz; stale input is independently
stopped by the library watchdog.

## Mapping and calibration

Print a starting configuration with `--print-default-config`, save it, and load it
with `--config FILE`. Version 1 supports `dead_zone`, `precision_scale`,
`keyboard_speed`, three inversion flags, and `pan_axis`, `tilt_axis`, and
`zoom_axis`. Axis names are `leftx`, `lefty`, `rightx`, `righty`, `left_trigger`,
and `right_trigger`. Unknown versions, wrong field types, invalid axes, and unsafe
ranges are rejected.

Run `--calibrate` without a camera. Release the controls and increase `dead_zone`
until drift reads zero, then exercise each axis to verify mapping, sign, and range.
Use `--raw-joystick` for devices without a useful SDL gamepad mapping. Add a proper
SDL mapping when possible because logical gamepad axes are more portable than raw
indices.

Camera-specific controller settings belong in application/device profiles:
command interval and refresh, host versus firmware ramping, minimum effective speed,
soft limits, keep-out zones, and the zoom-speed curve. Limits require fresh reliable
position feedback; they are an operational aid, not a substitute for mechanical
limits or an emergency Stop.

## Qualification and performance budgets

Qualify each camera at wide, middle, and telephoto zoom. Record input-to-command and
request latency, command rate, Stop latency, overshoot, fine-position accuracy,
audible motor behavior, CPU, memory, and whether status remains reliable. Recommended
starting budgets are: input-to-intent below 25 ms, intent-to-command below 150 ms,
Stop request initiation below 100 ms after loss, no more than 15 movement requests/s,
one pending intent, and no unbounded queues. Device network latency may require a
larger end-to-end Stop budget and must be reported rather than hidden.

Repeat focus loss, Bluetooth sleep/disconnect/reconnect, hot-plug replacement,
simultaneous keyboard/gamepad input, noisy-axis drift, emergency Stop, and shutdown.
Do not qualify near people or obstructions. Start at low maximum speeds with an
operator able to remove power if Stop is ineffective.

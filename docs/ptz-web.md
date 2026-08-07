# Browser PTZ control

`wlonvif-ptz-web` is an optional, narrow Boost.Beast server and embedded browser
frontend for `PtzController`. It is not a general web server and does not proxy
camera video or camera credentials.

## Build and run

```sh
cmake -S . -B build-web -DWLONVIF_BUILD_PTZ_WEB=ON
cmake --build build-web --target wlonvif-ptz-web
./build-web/examples/ptz-web/wlonvif-ptz-web --diagnose-input
```

Diagnostic mode serves only on `127.0.0.1:8080`. Open the printed URL; the browser
creates its short-lived session automatically. For live control, provide the usual
camera address, profile, credential options and
`--confirm-movement`. The camera password remains in the existing
`WLONVIF_PASSWORD` environment variable; it is never sent to the browser.

The video URL field accepts a browser-playable HTTP(S) source. Playback codec,
CORS and camera authentication are browser/source concerns. They do not affect
the control channel.

For the connected camera, the example can ask the ONVIF Media service for the
selected profile's stream URI, add the already-loaded camera credentials, launch
an installed FFmpeg, and serve a low-latency MJPEG conversion through the same
authenticated web server:

```sh
wlonvif-ptz-web --ip 192.0.2.20 --profile profile_1 --username operator \
  --confirm-movement --ffmpeg-stream
```

An explicit URL remains available for local relays, unusual cameras, and
diagnostic mode: `--ffmpeg-stream 'rtsp://relay.example/stream'`.

Use `--ffmpeg PATH` when FFmpeg is not on `PATH`. The process is spawned directly
without a shell, its standard input and diagnostics are closed, frames are bounded
to 4 MiB, and shutdown terminates and reaps it. RTSP uses TCP and is converted to
15 FPS, video-only MJPEG. Failure to find FFmpeg is reported at startup; later
stream/authentication failures appear as an empty video surface.

The discovered or explicit stream URI is passed to FFmpeg as an argument. The
program prints a warning without printing the URI. Embedding credentials in that
URI may expose them to process inspection by the same operating-system user and
may also put them in shell history. Prefer a camera-issued temporary stream URI or
a locally protected source. The bridge currently supports POSIX systems; Windows
returns a clear unsupported error. MJPEG is intentionally the smallest useful
bridge, but uses more bandwidth/CPU than retaining the source codec and carries no
audio. It is not intended as the eventual production video architecture.

## Input and automatic movement

The default viewport is a virtual joystick. Enable **Click/box mode** to make a
click request bounded centering, or drag a rectangle for centering plus zoom.
**Reverse click pan** and **Reverse click tilt** provide visible, browser-local
camera-direction adaptations. They affect only click and box operations and are
remembered by that browser; they do not reverse keyboard, button or virtual-
joystick input. Do not enable a checkbox when the same direction is already
reversed by a viewport calibration file, because the two inversions cancel.

The server queries all ONVIF Media profiles and their PTZ spaces at startup. The
profile selector performs a Stop, closes the old controller, and creates a new
controller for the selected token. Controls are dimmed and disabled when the
selected profile lacks the operation they require: manual pan/tilt and zoom need
continuous spaces, click centering needs relative pan/tilt, and box zoom also
needs relative zoom. Preset controls are enabled only when presets were returned.
The server enforces the same capability checks, so browser manipulation cannot
send an unsupported axis. If FFmpeg URI discovery is enabled, its video stream
remains the startup profile's stream; changing the PTZ profile does not restart
FFmpeg in this deliberately small example.
The persistent Stop control, Space, focus loss, tab hiding, WebSocket loss and
lease expiry all request Stop. Fresh manual input invalidates an active automatic
operation. Preset tours have bounded preset count, dwell and repeats and check for
cancellation at short intervals.

Click and box operations use conservative approximate `RelativeMove` unless a
reviewed calibration file is supplied:

```json
{
  "version": 1,
  "calibrated": true,
  "content_x": 0.0,
  "content_y": 0.1,
  "content_width": 1.0,
  "content_height": 0.8,
  "rotation": 0,
  "mirror_horizontal": false,
  "mirror_vertical": false,
  "pan_gain": 0.4,
  "tilt_gain": 0.3,
  "box_zoom_gain": 0.7,
  "maximum_translation": 0.4,
  "maximum_age_ms": 60000
}
```

Pass it with `--viewport-calibration FILE`. Content fractions describe the
actual image inside the displayed viewport and account for letter/pillarboxing.
Unknown versions/fields, invalid geometry and stale calibration are rejected or
degraded to bounded conservative movement. Rotation and mirroring are applied
before calibrated gains. Calibration files contain no secrets.

For manual direction- and zoom-dependent calibration, add up to 32 strictly
increasing normalized zoom samples:

```json
{
  "version": 1,
  "calibrated": true,
  "rotation": 0,
  "mirror_horizontal": true,
  "mirror_vertical": false,
  "pan_gain": 0.25,
  "tilt_gain": 0.25,
  "box_zoom_gain": 0.4,
  "maximum_translation": 0.3,
  "maximum_age_ms": 86400000,
  "zoom_points": [
    {
      "zoom": -1.0,
      "pan_left_gain": 0.28,
      "pan_right_gain": 0.25,
      "tilt_up_gain": 0.24,
      "tilt_down_gain": 0.27
    },
    {
      "zoom": 0.0,
      "pan_left_gain": 0.17,
      "pan_right_gain": 0.16,
      "tilt_up_gain": 0.15,
      "tilt_down_gain": 0.16
    },
    {
      "zoom": 1.0,
      "pan_left_gain": 0.06,
      "pan_right_gain": 0.06,
      "tilt_up_gain": 0.05,
      "tilt_down_gain": 0.05
    }
  ]
}
```

The controller telemetry includes `reported_zoom`. When it is reliable, the
mapper clamps to the endpoint samples or linearly interpolates between adjacent
samples. With exactly one sample, its directional gains are used even when the
camera reports no zoom position. With multiple samples and no reliable zoom, the
top-level `pan_gain` and `tilt_gain` remain the safe fallback.

A practical manual workflow is:

1. Begin with conservative maximum translation and one zoom point.
2. At a fixed zoom, click targets separately to the left, right, top and bottom.
3. Measure the fraction of the original screen error that the move corrected.
4. Divide that direction's old gain by the corrected fraction; for example, a
   move that corrects half the error needs approximately twice the gain.
5. Repeat with smaller errors, then add ordered wide/middle/telephoto samples if
   the camera supplies reliable normalized zoom.
6. Retest the viewport edges and retain bounds that cannot cause unsafe travel.

These values are empirical because generic ONVIF translation spaces do not state
degrees per normalized unit. Applications own persistence and review of the JSON;
the library owns validation and deterministic interpolation.

## Remote security and threat model

The default loopback HTTP/WS mode assumes the local desktop is trusted. A hostile
local process could still control the browser or make requests from an allowed
origin. Sessions are random, short-lived, HttpOnly, SameSite cookies.
Every command is size/rate bounded, strict-schema parsed, Origin allowlisted,
monotonically sequenced and tied to a renewable owner lease.

Non-loopback binding is refused unless all of these are supplied:

- `--allow-remote`;
- `--tls-cert FILE` and `--tls-key FILE` for HTTPS/WSS;
- exact `--origin https://operator.example` allowlist entries.

The example has no user authentication. An Origin allowlist is a browser request
boundary, not an identity check; any client that can reach a remote listener may
forge that header. Put remote deployments behind an authenticating reverse proxy
or keep the example bound to loopback.

Example:

```sh
wlonvif-ptz-web --diagnose-input --listen 192.0.2.10 --port 8443 \
  --allow-remote --tls-cert server.pem --tls-key server.key \
  --origin https://192.0.2.10:8443
```

TLS 1.0 and 1.1 are disabled. Requests, messages, sessions and concurrent
connections are bounded; assets are compiled in and no filesystem route exists.
Security headers deny framing and restrict scripts to the embedded nonce.
Deployment behind identity infrastructure, certificate issuance/rotation and
multi-camera authorization are deliberately outside this example. FFmpeg video is
served only after session-cookie validation and is never exposed on a second port.

## Qualification and limitations

Protocol, mapper, malformed input, replay, rate, lease, disconnect, remote-bind,
bad-certificate and orderly-shutdown behavior are deterministic tests. Real
camera centering accuracy, settling/status quality, browser/gamepad behavior and
TLS packaging must still be qualified on each supported platform and camera.
Approximate movement is explicitly bounded but cannot infer lens distortion or
physical field of view. Tracking is not implemented; it should be a separate
cancellable client of the same controller arbitration rather than browser logic.

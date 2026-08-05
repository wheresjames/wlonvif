# wlonvif

`wlonvif` is a C++20 library for discovering, inspecting, and controlling ONVIF
devices. It includes a network client, a deterministic virtual-device backend, and
command-line examples for common camera workflows.

The project currently supports Device operations, Media1 profiles and URIs,
normalized PTZ control, WS-Discovery and direct network probing, PullPoint events,
bounded SOAP/XML processing, endpoint authorization, and authenticated HTTP
transport.

> **Project status:** `wlonvif` is under active development. Test with the devices
> and firmware versions you intend to deploy, because ONVIF implementations vary
> considerably between vendors.

## Table of contents

- [Highlights](#highlights)
- [Requirements](#requirements)
- [Quick start](#quick-start)
- [Build options](#build-options)
- [Testing](#testing)
- [Using the library](#using-the-library)
- [Connecting to real devices](#connecting-to-real-devices)
  - [Device addresses](#device-addresses)
  - [Credentials](#credentials)
  - [Common options](#common-options)
- [Examples](#examples)
  - [Discover devices](#discover-devices)
  - [Read device information](#read-device-information)
  - [Build a camera inventory](#build-a-camera-inventory)
  - [Inspect endpoint policy](#inspect-endpoint-policy)
  - [Use the virtual device](#use-the-virtual-device)
  - [Use media profiles, streams, and snapshots](#use-media-profiles-streams-and-snapshots)
  - [Inspect and control PTZ](#inspect-and-control-ptz)
  - [Monitor events](#monitor-events)
  - [Check camera health](#check-camera-health)
  - [Inspect multiple cameras](#inspect-multiple-cameras)
- [Real-device smoke testing](#real-device-smoke-testing)
- [API documentation](#api-documentation)
- [Installing and consuming wlonvif](#installing-and-consuming-wlonvif)
- [Security and interoperability](#security-and-interoperability)

## Highlights

- Modern C++20 API with cold asynchronous tasks and explicit cancellation/deadlines.
- Device, Media1, PTZ, PullPoint Event, and WS-Discovery support.
- Robust discovery using multicast, targeted unicast, and bounded direct probing.
- HTTP and WS-Security authentication support without logging secrets.
- Endpoint policies that prevent devices from redirecting clients to unauthorized
  hosts.
- Bounded XML, response, discovery queue, and event queue processing.
- Deterministic virtual transport for tests and development without camera hardware.
- Static/shared builds, sanitizers, fuzzing support, Doxygen, install packages, and
  standalone consumer verification.

## Requirements

Building the core libraries requires:

- CMake 3.24 or newer
- A C++20 compiler
- libcurl 8 or newer
- libxml2 2.9 or newer
- OpenSSL 3 or newer
- A platform threading implementation

Doxygen and Graphviz `dot` are additionally required when
`WLONVIF_BUILD_DOCS=ON`. LibFuzzer targets require Clang.

VLC, mpv, and FFmpeg are not library dependencies. The media example only prints
commands for these external programs; snapshot download is implemented by the
library and needs no image codec.

## Quick start

The simplest development build enables every component supported by the selected
compiler:

```sh
cmake -S . -B build -DWLONVIF_BUILD_EVERYTHING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Discover cameras on the local network:

```sh
./build/examples/discover/wlonvif-discover
```

If you already know a camera address, targeted discovery is usually faster and more
reliable than multicast:

```sh
./build/examples/discover/wlonvif-discover \
  --target CAMERA_IP --thorough --timeout 15
```

Read its identity and service information:

```sh
./build/examples/device-info/wlonvif-device-info \
  --ip CAMERA_IP --all --username CAMERA_USER \
  --allow-cleartext-basic
```

If the password is not available from the environment, the example prompts for it
without echoing it.

## Build options

A normal top-level build includes the libraries and tests:

```sh
cmake -S . -B build -DWLONVIF_BUILD_TESTING=ON
cmake --build build --parallel
```

Important CMake options are:

| Option | Default | Purpose |
|---|---:|---|
| `WLONVIF_BUILD_EVERYTHING` | `OFF` | Enable tests, examples, documentation, documentation installation, and Clang fuzzers where supported. |
| `WLONVIF_BUILD_TESTING` | top-level `ON` | Build the deterministic test suite. |
| `WLONVIF_BUILD_EXAMPLES` | `OFF` | Build all command-line examples. |
| `WLONVIF_BUILD_DOCS` | `OFF` | Generate the Doxygen HTML reference. |
| `WLONVIF_INSTALL_DOCS` | `OFF` | Install generated HTML documentation. |
| `WLONVIF_BUILD_FUZZERS` | `OFF` | Build the XML libFuzzer target; requires Clang. |
| `WLONVIF_ENABLE_SANITIZERS` | `OFF` | Enable AddressSanitizer and UndefinedBehaviorSanitizer with GCC or Clang. |
| `WLONVIF_ENABLE_HARDENING` | `ON` | Enable supported compiler and linker hardening. |
| `WLONVIF_WARNINGS_AS_ERRORS` | `OFF` | Treat project compiler warnings as errors. |

`WLONVIF_BUILD_EVERYTHING` skips fuzz targets on non-Clang compilers rather than
failing configuration. Sanitizers remain a separate build because normal and
instrumented objects should not share a build tree.

## Testing

Run the tests configured in an existing build tree:

```sh
ctest --test-dir build --output-on-failure
```

For release qualification, run the complete static, shared, sanitizer, install, and
consumer matrix:

```sh
./scripts/test-all.sh
```

The regular test suite is deterministic and does not contact real cameras.
LeakSanitizer is disabled by the script because it cannot operate in some managed or
ptraced environments. Enable it in an unrestricted runner with:

```sh
ASAN_OPTIONS=detect_leaks=1 ./scripts/test-all.sh
```

## Using the library

The public API is provided by:

```cpp
#include <wlonvif/wlonvif.hpp>
```

Applications normally create one shared `Runtime`, construct an HTTP transport,
select an endpoint policy, and connect a `DeviceSession`. A connected session owns
the authorized Device endpoint and exposes clients for Media, PTZ, and Events.

Operations return cold `Task<Result<T>>` values: work begins when the task is
started. Each network operation accepts `CallOptions`, allowing the caller to set a
deadline and cancellation token. See the examples for complete synchronous command-
line adapters around these asynchronous tasks.

The libraries exported to CMake consumers are:

- `wlonvif::core` — runtime primitives, XML, SOAP, security, and shared types.
- `wlonvif::client` — HTTP transport, network discovery, and ONVIF service clients.
- `wlonvif::device` — deterministic virtual-device transport.

## Connecting to real devices

### Device addresses

Real-device examples accept exactly one of:

- `--ip ADDRESS` — perform targeted discovery and direct probing to find the Device
  service URL, including nonstandard ports.
- `--url DEVICE_URL` — use a known Device service URL directly.

Prefer `--ip` for ordinary use:

```sh
--ip CAMERA_IP
```

Use `--url` when discovery is blocked or a device advertises the wrong endpoint:

```sh
--url http://CAMERA_IP:2020/onvif/device_service
```

### Credentials

Examples resolve the username from `--username` and then `WLONVIF_USERNAME`. They
resolve the password in this order:

1. `--password PASSWORD`
2. `--password-file FILE`
3. `WLONVIF_PASSWORD`, or the variable selected by `--password-env NAME`
4. An interactive password prompt

If no credentials are initially available, the examples first try anonymous access.
On an HTTP or SOAP authentication failure, an interactive terminal prompts and
retries once. Redirected or scripted input never blocks waiting for a prompt.

A convenient interactive invocation is:

```sh
./build/examples/media/wlonvif-media \
  --ip CAMERA_IP profiles --username CAMERA_USER \
  --allow-cleartext-basic
```

For automation, keep the password out of process arguments:

```sh
export WLONVIF_USERNAME='CAMERA_USER'
export WLONVIF_PASSWORD='CAMERA_PASSWORD'
./build/examples/media/wlonvif-media --ip CAMERA_IP profiles
```

`--password-file` requires a file that is not accessible to group or other users.
HTTP Basic authentication over unencrypted HTTP is rejected unless
`--allow-cleartext-basic` is explicitly supplied.

### Common options

Most real-device examples support:

| Option | Meaning |
|---|---|
| `--timeout SECONDS` | Set the operation deadline. |
| `--username NAME` | Set the ONVIF username. |
| `--password PASSWORD` | Set the password; convenient but visible in shell history and process listings. |
| `--password-file FILE` | Read a password from a protected file. |
| `--password-env NAME` | Read the password from a named environment variable. |
| `--allow-cleartext-basic` | Permit HTTP Basic credentials over cleartext HTTP. |
| `--json` | Emit machine-readable JSON where supported. |
| `--no-progress` | Disable progress output where supported. |
| `--help` | Show the executable's authoritative option summary. |

## Examples

All examples are enabled by `WLONVIF_BUILD_EXAMPLES=ON` and are placed under
`build/examples/<name>/`. They are intentionally small applications that demonstrate
library behavior rather than production camera-management tools.

### Discover devices

`wlonvif-discover` combines several discovery strategies and merges duplicate
observations into one candidate per device.

Basic multicast discovery sends current, legacy, and unfiltered WS-Discovery probes
on each eligible IPv4 interface:

```sh
./build/examples/discover/wlonvif-discover --timeout 8
```

Inspect a known address using unicast discovery and common Device service paths:

```sh
./build/examples/discover/wlonvif-discover \
  --target CAMERA_IP --thorough --timeout 15
```

Scan an authorized subnet, or automatically scan directly connected networks that
fit the host limit:

```sh
./build/examples/discover/wlonvif-discover \
  --scan CAMERA_CIDR --thorough --timeout 30 --jobs 32

./build/examples/discover/wlonvif-discover \
  --scan auto --max-hosts 1024 --timeout 30
```

Useful options include `--interface ADDRESS` (repeatable), `--no-multicast`,
`--max-hosts COUNT`, `--jobs COUNT`, `--json`, and `--no-progress`. Scanning is
bounded and opt-in. Only scan addresses you are authorized to inspect.

If multicast finds nothing, check host and network firewall rules for UDP port 3702,
then use `--target` or `--scan`. Targeted discovery also handles cameras whose
ONVIF service runs on a nonstandard HTTP port.

### Read device information

`wlonvif-device-info` performs read-only Device service calls:

```sh
./build/examples/device-info/wlonvif-device-info \
  --ip CAMERA_IP --all --username CAMERA_USER \
  --allow-cleartext-basic
```

Select individual sections with `--identity`, `--clock`, `--scopes`, `--services`,
and `--capabilities`. With no section option, all sections are shown. Add `--json`
for structured output.

### Build a camera inventory

`wlonvif-inventory` discovers devices and queries them concurrently:

```sh
./build/examples/inventory/wlonvif-inventory \
  --scan CAMERA_CIDR --timeout 30 --jobs 8 \
  --username CAMERA_USER --allow-cleartext-basic
```

Use repeatable `--target ADDRESS` options for a known set, or `--scan auto` for
eligible local networks. `--media`, `--ptz`, and `--events` add optional service
columns and additional network calls. A failed device is reported independently and
does not discard successful inventory rows.

### Inspect endpoint policy

`wlonvif-endpoint-policy` demonstrates URL authorization without making network
requests. The default same-host policy allows a device to advertise another port on
the same host; `--strict` selects same-origin matching.

```sh
./build/examples/endpoint-policy/wlonvif-endpoint-policy \
  --origin http://camera/onvif/device_service \
  --candidate http://camera:8080/onvif/media \
  --candidate http://other-host/onvif/media
```

The process exits unsuccessfully if any candidate is rejected, which makes it useful
in scripts and policy tests.

### Use the virtual device

`wlonvif-virtual-device` demonstrates Device, Media, PTZ preset, and Event operations
without network access or camera hardware:

```sh
./build/examples/virtual-device/wlonvif-virtual-device
```

Its in-memory backend is deterministic and can be configured with profiles, presets,
and emitted events. This is the best example to start from when writing unit tests
for an application built on `wlonvif`.

### Use media profiles, streams, and snapshots

First list the camera's Media profiles:

```sh
./build/examples/media/wlonvif-media \
  --ip CAMERA_IP profiles --username CAMERA_USER \
  --allow-cleartext-basic
```

The first column is the profile token required by the other Media and PTZ commands:

```text
profile_1  mainStream
profile_2  minorStream
profile_3  jpegStream
```

Retrieve a playable RTSP URI and command templates:

```sh
./build/examples/media/wlonvif-media \
  --ip CAMERA_IP stream-uri --profile profile_1 \
  --username CAMERA_USER --allow-cleartext-basic
```

When credentials are supplied or obtained from an authentication prompt, generated
RTSP links include percent-encoded user information so the VLC, mpv, and FFmpeg
commands are directly usable. This necessarily prints the cleartext password; do not
copy this output into logs or issue reports.

Some VLC distributions lack a general RTSP input module. In that case, use the
printed FFmpeg URI with `ffplay`:

```sh
ffplay -rtsp_transport tcp 'rtsp://USER:PASSWORD@CAMERA/STREAM'
```

Print, but do not execute, an FFmpeg stream-copy recording command:

```sh
./build/examples/media/wlonvif-media \
  --ip CAMERA_IP record-command --profile profile_1 \
  --output recording.mkv --username CAMERA_USER --allow-cleartext-basic
```

Get a snapshot URI or download the image directly:

```sh
./build/examples/media/wlonvif-media \
  --ip CAMERA_IP snapshot-uri --profile profile_1 \
  --username CAMERA_USER --allow-cleartext-basic

./build/examples/media/wlonvif-media \
  --ip CAMERA_IP snapshot --profile profile_1 \
  --output snapshot.jpg --max-bytes 16777216 \
  --username CAMERA_USER --allow-cleartext-basic
```

Snapshot download requires no codec. It verifies an image content type, writes a
temporary `.part` file, and renames it only after a complete response. Existing
output files are never overwritten.

### Inspect and control PTZ

PTZ commands require a Media profile token. Start with read-only operations:

```sh
./build/examples/ptz/wlonvif-ptz \
  --ip CAMERA_IP --profile profile_1 status \
  --username CAMERA_USER --allow-cleartext-basic

./build/examples/ptz/wlonvif-ptz \
  --ip CAMERA_IP --profile profile_1 presets \
  --username CAMERA_USER --allow-cleartext-basic
```

Movement is opt-in: every movement command requires `--confirm-movement`, normalized
axes are clamped to `[-0.25, +0.25]`, and independent safety Stop attempts are made
on completion and expected error/exit paths.

Perform a small relative pan:

```sh
./build/examples/ptz/wlonvif-ptz \
  --ip CAMERA_IP --profile profile_1 move --pan 0.05 \
  --username CAMERA_USER --allow-cleartext-basic --confirm-movement
```

Add `--continuous --duration-ms 750` for a time-bounded continuous movement. Use
`--tilt` and `--zoom` for the other axes.

Move to a preset or visit a short preset tour:

```sh
./build/examples/ptz/wlonvif-ptz \
  --ip CAMERA_IP --profile profile_1 goto-preset --preset home \
  --username CAMERA_USER --allow-cleartext-basic --confirm-movement

./build/examples/ptz/wlonvif-ptz \
  --ip CAMERA_IP --profile profile_1 tour \
  --preset entrance --preset driveway --duration-ms 2000 \
  --username CAMERA_USER --allow-cleartext-basic --confirm-movement
```

Interactive keyboard control works in a real terminal:

```sh
./build/examples/ptz/wlonvif-ptz \
  --ip CAMERA_IP --profile profile_1 keyboard --speed 0.10 \
  --username CAMERA_USER --allow-cleartext-basic --confirm-movement
```

Use WASD or cursor keys for pan/tilt, `+` and `-` for zoom, Space to Stop, and Q to
exit. Direction changes send movement commands; keyboard repeats are coalesced and
extend a short lease. Releasing the key infers one Stop. Tune terminal behavior with
`--initial-hold-ms` (default 550) and `--release-ms` (default 180). If autorepeat is
unreliable, `--latched` keeps moving until another direction, Space, or Q is pressed.
The ASCII position marker refreshes after Stop so status calls cannot delay the
safety loop while the camera is moving.

### Monitor events

`wlonvif-event-monitor` creates and manages an ONVIF PullPoint subscription:

```sh
./build/examples/event-monitor/wlonvif-event-monitor \
  --ip CAMERA_IP --topic Motion --count 10 \
  --username CAMERA_USER --allow-cleartext-basic
```

Omit `--count` to continue until Ctrl-C. `--topic TEXT` filters topic names. Event
JSON is emitted as one object per line when `--json` is enabled, while progress and
diagnostics remain on stderr. The managed stream has a bounded queue and reports
health/reconnection signals separately from device events.

### Check camera health

`wlonvif-camera-health` runs read-only operational checks across available service
domains and reports details such as discovery evidence, identity, clock skew, Media,
PTZ, and Event availability:

```sh
./build/examples/camera-health/wlonvif-camera-health \
  --ip CAMERA_IP --username CAMERA_USER --allow-cleartext-basic
```

Use `--json` for monitoring or automation. A warning may indicate an unsupported
optional service rather than a completely unreachable camera, so retain the detail
field when processing results.

### Inspect multiple cameras

`wlonvif-multi-camera` queries an explicit trusted set with bounded concurrency:

```sh
./build/examples/multi-camera/wlonvif-multi-camera --jobs 4 \
  --ip CAMERA_A_IP \
  --url http://camera-b/onvif/device_service \
  --username CAMERA_USER --allow-cleartext-basic
```

Repeat `--ip` and `--url` as needed. A failed or slow camera is represented by its
own result row and does not remove successful results from other devices. The same
credentials are currently used for every supplied device.

## Real-device smoke testing

The opt-in hardware smoke script performs read-only Device and health checks:

```sh
WLONVIF_DEVICE_IP=CAMERA_IP \
WLONVIF_USERNAME='CAMERA_USER' \
WLONVIF_PASSWORD='CAMERA_PASSWORD' \
./scripts/test-real-device.sh
```

Set `WLONVIF_DEVICE_URL` instead of `WLONVIF_DEVICE_IP` to bypass endpoint discovery.
Optional checks are enabled with:

- `WLONVIF_TEST_MEDIA=1` — list Media profiles.
- `WLONVIF_TEST_PTZ_STATUS=1` — read PTZ status; also set
  `WLONVIF_PROFILE_TOKEN`.
- `WLONVIF_TEST_PTZ_MOVE=1` — perform an explicitly enabled small PTZ movement; also
  set `WLONVIF_PROFILE_TOKEN`.
- `WLONVIF_TIMEOUT_SECONDS` — override the default 15-second device timeout.
- `WLONVIF_BUILD_DIR` — select a build directory other than `build`.

## API documentation

Generate the Doxygen HTML reference with:

```sh
cmake -S . -B build-docs -DWLONVIF_BUILD_DOCS=ON
cmake --build build-docs --target wlonvif_docs
```

Open `build-docs/docs/html/index.html` in a browser. Documentation warnings fail the
build. The generated pages and graphs use the project's dark theme.

Set `WLONVIF_INSTALL_DOCS=ON` to install the generated HTML under the platform CMake
documentation directory.

## Installing and consuming wlonvif

Install to a chosen prefix:

```sh
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/desired/prefix
cmake --build build-release --parallel
cmake --install build-release
```

A downstream CMake project can then use the exported targets:

```cmake
find_package(wlonvif 0.1 CONFIG REQUIRED)

add_executable(camera_app main.cpp)
target_link_libraries(camera_app PRIVATE wlonvif::client)
target_compile_features(camera_app PRIVATE cxx_std_20)
```

Link `wlonvif::device` when the application also uses the virtual transport.

## Security and interoperability

- Run discovery and subnet scans only on networks you own or are authorized to
  inspect.
- Prefer HTTPS or WS-Security over HTTP Basic authentication. Cleartext Basic auth
  requires explicit opt-in, but that does not make the connection encrypted.
- Prefer the no-echo prompt, environment, or a protected password file over
  `--password`; arguments can be exposed through process inspection and shell
  history.
- Playable RTSP URLs necessarily expose embedded credentials in terminal output,
  logs, and process listings. Treat them as secrets and rotate credentials if a URL
  is shared.
- HTTPS certificate verification remains enabled. Trust a private CA or device
  certificate locally rather than disabling verification.
- Default endpoint policies restrict service URLs advertised by a device, reducing
  unintended cross-host requests.
- Discovery depends on UDP multicast, routing, interfaces, and firewall policy.
  Direct targets and authorized CIDR probing remain important fallbacks.
- ONVIF provides RTSP URIs but `wlonvif` does not implement RTP depacketization,
  codecs, or media containers. Use a media application such as FFmpeg for playback
  or recording.
- Vendors differ in namespaces, authentication, ports, PTZ coordinate spaces,
  optional services, and Event behavior. Validate important workflows on each target
  model and firmware release.

## License

See [LICENSE](LICENSE).

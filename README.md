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
- [Comparison to similar projects](#comparison-to-similar-projects)
  - [Compared with gSOAP and generated ONVIF clients](#compared-with-gsoap-and-generated-onvif-clients)
  - [Compared with Privatehive/libONVIF](#compared-with-privatehivelibonvif)
  - [Compared with OnvifClient_gSOAP](#compared-with-onvifclient_gsoap)
  - [Compared with onvif-qt-server-client](#compared-with-onvif-qt-server-client)
  - [Compared with ONVIF-HTTP-code](#compared-with-onvif-http-code)
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

VLC, mpv, and FFmpeg are not library dependencies. The media example prints
commands for external players, while the optional browser PTZ example can launch an
installed FFmpeg at runtime for its simple MJPEG bridge. Snapshot download is
implemented by the library and needs no image codec. SDL3 is required only by the
native PTZ example; Boost 1.89 with JSON is required only by the browser example.

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
| `WLONVIF_BUILD_EVERYTHING` | `OFF` | Enable tests, examples including native SDL3 and browser PTZ control, documentation, documentation installation, and Clang fuzzers where supported. An installed SDL3 is preferred; otherwise its pinned source is fetched. |
| `WLONVIF_BUILD_TESTING` | top-level `ON` | Build the deterministic test suite. |
| `WLONVIF_BUILD_EXAMPLES` | `OFF` | Build the dependency-light command-line examples. |
| `WLONVIF_BUILD_PTZ_CONTROL` | `OFF` | Build the optional SDL3 native keyboard/gamepad PTZ example. |
| `WLONVIF_BUILD_PTZ_WEB` | `OFF` | Build the optional Boost.Beast browser PTZ example. |
| `WLONVIF_BUILD_PTZ_DEPENDENCY_SPIKES` | `OFF` | Build the SDL3 and Beast acceptance probes. |
| `WLONVIF_FETCH_PTZ_DEPENDENCIES` | `OFF` | Fetch the pinned SDL3 release instead of requiring a system package. |
| `WLONVIF_BUILD_DOCS` | `OFF` | Generate the Doxygen HTML reference. |
| `WLONVIF_INSTALL_DOCS` | `OFF` | Install generated HTML documentation. |
| `WLONVIF_BUILD_FUZZERS` | `OFF` | Build the XML and optional web-protocol libFuzzer targets; requires Clang. |
| `WLONVIF_ENABLE_SANITIZERS` | `OFF` | Enable AddressSanitizer and UndefinedBehaviorSanitizer with GCC or Clang. |
| `WLONVIF_ENABLE_THREAD_SANITIZER` | `OFF` | Enable ThreadSanitizer with GCC or Clang; cannot be combined with `WLONVIF_ENABLE_SANITIZERS`. |
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

For release qualification, run the complete static, shared, sanitizer, thread
sanitizer, install, consumer, and fuzz-smoke matrix:

```sh
./scripts/test-all.sh
```

The regular test suite is deterministic and does not contact real cameras.
LeakSanitizer is disabled by the script because it cannot operate in some managed or
ptraced environments. Enable it in an unrestricted runner with:

```sh
ASAN_OPTIONS=detect_leaks=1 ./scripts/test-all.sh
```

The script ends with a short libFuzzer run over both targets when `clang++` is
available. It is a regression gate rather than a fuzzing campaign; lengthen it with:

```sh
WLONVIF_FUZZ_SECONDS=300 ./scripts/test-all.sh
```

The same matrix runs in CI on every push and pull request, including a dedicated
ThreadSanitizer job. Races on the completion paths are reported by no other
sanitizer, so that job is not redundant with the address/UB one.

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

The dependency-light examples are enabled by `WLONVIF_BUILD_EXAMPLES=ON` and are
placed under `build/examples/<name>/`. Native and browser PTZ frontends additionally
require `WLONVIF_BUILD_PTZ_CONTROL=ON` and `WLONVIF_BUILD_PTZ_WEB=ON`, respectively;
`WLONVIF_BUILD_EVERYTHING=ON` enables both. Examples are intentionally focused
applications that demonstrate library behavior rather than production
camera-management tools.

### Discover devices

`wlonvif-discover` combines several discovery strategies and merges duplicate
observations into one candidate per device.

Basic multicast discovery sends current, legacy, and unfiltered WS-Discovery probes
on each eligible IPv4 interface:

```sh
./build/examples/discover/wlonvif-discover --timeout 8
```

When camera addresses are unknown, use the high-coverage local discovery preset.
It combines multicast on every eligible interface with direct inspection and
Device-service verification across eligible directly connected networks:

```sh
./build/examples/discover/wlonvif-discover --discover-all
```

`--discover-all` defaults to a 60-second overall deadline and 32 simultaneous
direct probes. It prints the directly scanned CIDRs before starting. Networks
larger than the `--max-hosts` safety bound (1024 by default) are not scanned;
specify a larger authorized bound or an explicit CIDR when appropriate:

```sh
./build/examples/discover/wlonvif-discover \
  --discover-all --max-hosts 65536 --timeout 180 --jobs 64
```

No local discovery mode can inspect unknown routed VLANs or networks hidden by
firewall policy. Add those authorized networks explicitly with `--scan CIDR`.

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

./build/examples/ptz/wlonvif-ptz \
  --ip CAMERA_IP --profile profile_1 spaces \
  --username CAMERA_USER --allow-cleartext-basic
```

Movement is opt-in: every movement command requires `--confirm-movement`, normalized
axes must be in `[-1, +1]`, and a timed continuous movement is followed by an
independent emergency Stop request.

Perform a small relative pan:

```sh
./build/examples/ptz/wlonvif-ptz \
  --ip CAMERA_IP --profile profile_1 move --relative --pan 0.05 \
  --username CAMERA_USER --allow-cleartext-basic --confirm-movement
```

Omit `--relative` for a time-bounded continuous movement and select its bound with
`--duration-ms 750`. Use `--tilt` and `--zoom` for the other axes. `absolute` uses
the device's advertised absolute position space when available.

Move to an absolute position or preset:

```sh
./build/examples/ptz/wlonvif-ptz \
  --ip CAMERA_IP --profile profile_1 absolute --pan 0 --tilt 0 \
  --username CAMERA_USER --allow-cleartext-basic --confirm-movement

./build/examples/ptz/wlonvif-ptz \
  --ip CAMERA_IP --profile profile_1 goto-preset --preset home \
  --username CAMERA_USER --allow-cleartext-basic --confirm-movement
```

The diagnostic CLI intentionally has no interactive terminal mode: portable terminal
input cannot report reliable key release. Applications should use `PtzController`,
which provides latest-intent coalescing, acceleration/deceleration ramps, response
curves and dead zones, zoom-aware pan/tilt speed, watchdog and maximum-run stops,
bounded device command rate, ownership sequences, telemetry, and explicit close.
`ViewportMapper` separately provides bounded click-to-center and box-zoom geometry,
including crop/rotation/mirroring and direction-specific gains interpolated across
manually calibrated zoom samples.
See `docs/ptz-phase1.md` for its architecture and qualification workflow.
The `qualify` command performs one explicitly confirmed move limited to `0.25`,
always requests emergency Stop, and reports request latencies and before/after status;
it never performs an unattended sweep.

For native keyboard, mapped-gamepad, and raw-joystick control, configure with
`WLONVIF_BUILD_PTZ_CONTROL=ON`. The optional `wlonvif-ptz-control` executable uses
SDL3 for real press/release and hot-plug state while leaving movement policy in
`PtzController`. It also provides camera-free input diagnostics and calibration.
See [Native PTZ control](docs/ptz-control.md) for build, mapping, safety, and
qualification guidance.

For the loopback-first browser controller, configure with
`WLONVIF_BUILD_PTZ_WEB=ON`. It provides keyboard, pointer/touch, external video,
click-to-center/box movement, presets and cancellable tours over a bounded
authenticated WebSocket protocol. It queries all Media profiles and their PTZ
spaces, permits safe live profile selection, and dims controls unsupported by the
active profile. Click direction can be reversed in the UI for nonconforming or
mirrored cameras. Versioned calibration JSON supports independent left/right/up/down
gains and optional interpolation across normalized zoom samples.
Load that configuration with `--viewport-calibration FILE`.

Build and run it with automatic profile selection and ONVIF stream discovery:

```sh
cmake -S . -B build-web \
  -DWLONVIF_BUILD_EXAMPLES=ON -DWLONVIF_BUILD_PTZ_WEB=ON
cmake --build build-web --target wlonvif-ptz-web

WLONVIF_PASSWORD='CAMERA_PASSWORD' \
./build-web/examples/ptz-web/wlonvif-ptz-web \
  --ip CAMERA_IP --username CAMERA_USER --confirm-movement --ffmpeg-stream
```

`--profile TOKEN` optionally chooses the startup profile; otherwise the first
advertised profile is used. `--ffmpeg-stream [URL]` discovers that profile's ONVIF
stream URI or accepts an explicit relay/source override, then launches an installed
FFmpeg to provide a video-only MJPEG bridge. FFmpeg is an optional POSIX runtime
integration and the input URI may be visible to same-user process inspection.
Changing the PTZ profile in the browser does not restart the startup FFmpeg stream.
The server defaults to loopback HTTP/WS; remote binding requires TLS, an explicit
Origin allowlist, and `--allow-remote`. The example has no user authentication, so
remote use should be placed behind an authenticating proxy. See
[Browser PTZ control](docs/ptz-web.md) for its security and calibration contract.

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

## Comparison to similar projects

There are several good ways to build ONVIF applications. The right choice depends
on the implementation language, how much of the ONVIF specification is needed, and
whether the project prefers generated schema bindings or a smaller curated API.

The following comparison was reviewed on 2026-08-05 and is based on the projects'
published documentation. It is not a conformance certification, security audit,
maintenance assessment, popularity ranking, or performance benchmark. Feature
support can change over time.

| Project | Language and approach | Published scope | Discovery | Distinguishing trade-off |
|---|---|---|---|---|
| **wlonvif** | C++20; curated typed API with hand-maintained SOAP/XML handling | Device reads, Media1 profiles and URIs, normalized PTZ, PullPoint Events, endpoint policy, and virtual devices | Multicast, targeted unicast, CIDR probing, duplicate suppression, and bounded concurrency | Small native API, explicit deadlines/cancellation, bounded inputs, and deterministic tests; it intentionally covers fewer ONVIF operations than schema-generated libraries. |
| [gSOAP](https://www.genivia.com/doc/guide/html/index.html) and generated ONVIF clients | C or C++; WSDL/XSD-to-code generation plus the gSOAP runtime | Potentially broad SOAP and ONVIF schema coverage determined by the imported WSDLs and plugins | WS-Discovery support is available through gSOAP tooling/plugins | Strong choice when broad, specification-shaped bindings or client/server generation matter; generated sources, the gSOAP runtime, and its GPLv2/commercial licensing choices add integration considerations. |
| [Privatehive/libONVIF](https://github.com/Privatehive/libONVIF) | C++ with Qt-facing high-level classes that hide most gSOAP details | Publishes eleven client services, including Media1/Media2, Imaging, Analytics, Recording, Replay, and PTZ | Dedicated `OnvifDiscovery` class | Much broader advertised service surface and natural Qt/QML fit; requires the Qt/gSOAP ecosystem and uses GPL-3.0 rather than MIT. |
| [OnvifClient_gSOAP](https://github.com/Allforgot/OnvifClient_gSOAP) | C++; ONVIF wrapper and examples built on generated gSOAP code | Its example demonstrates Device information, capabilities, Media, and PTZ operations | The repository describes remote discovery as unfinished | A concrete gSOAP-based starting point for C++ users, but its documented setup expects generated ONVIF code and exposes a less self-contained workflow than `wlonvif`. |
| [onvif-qt-server-client](https://github.com/hummatli/onvif-qt-server-client) | Qt C++ sample using gSOAP-generated service structures | Advertises client and server libraries across Device, Events, Imaging, Media, PTZ, Recording, Replay, and other domains | Includes an ONVIF Discovery library and client/server discovery demonstration | Includes both client and server reference code; its documented environment is Qt 5.5, Visual Studio 2013, and 32-bit Windows, so current-toolchain portability requires separate assessment. |
| [ONVIF-HTTP-code](https://github.com/fernandorpardo/ONVIF-HTTP-code) | C++; direct HTTP and XML implementation intended to show the protocol underneath | Partial implementation tested by its author with several TP-Link Tapo cameras, including PTZ examples | Not presented as a general robust discovery toolkit | Particularly useful as educational, readable protocol-level code and for its tested camera family; its own documentation explicitly says it is not comprehensive. |

The sections below expand on the table. “Choose” means “is likely a better starting
point,” not that one project is universally superior. Published service coverage is
not the same as ONVIF conformance, and none of these comparisons replaces testing on
the intended camera models and firmware.

### Compared with gSOAP and generated ONVIF clients

[gSOAP](https://www.genivia.com/doc/guide/html/index.html) is a general C/C++
SOAP/XML toolkit rather than an ONVIF-only client. Its `wsdl2h` and `soapcpp2` tools
turn WSDL and XSD definitions into C/C++ serializers, client stubs, and optional
server skeletons. ONVIF projects built on gSOAP can therefore expose a much larger
specification-shaped API than `wlonvif` without manually implementing every XML
message.

Key differences:

- gSOAP favors generated types and operations that closely reflect the imported
  schemas. `wlonvif` favors a smaller, hand-maintained API with normalized value
  types and selected workflows.
- gSOAP supports both client and server generation and a broad set of WS-* features.
  `wlonvif` is primarily a client library; its virtual device is an in-process test
  transport, not a general ONVIF network server.
- A gSOAP application owns its WSDL import, code-generation, typemap, plugin, and
  generated-source workflow. `wlonvif` builds directly from its checked-in source
  using ordinary CMake dependencies.
- `wlonvif` supplies project-specific limits, endpoint authorization, cold tasks,
  cancellation/deadlines, direct-scan discovery, and a deterministic test backend.
  Similar policies can be built around gSOAP, but they are application integration
  choices rather than the main abstraction advertised by the toolkit.
- gSOAP is offered as GPLv2 open source and under commercial licenses. `wlonvif` is
  MIT-licensed. License compatibility may decide the choice before technical details
  do.

Choose gSOAP-generated bindings when broad WSDL coverage, native representation of
many ONVIF request/response types, server generation, or advanced SOAP/WS-* tooling
is important. Choose `wlonvif` when its narrower supported operations are sufficient
and a compact C++20 API, bounded behavior, discovery fallbacks, and testability are
more valuable than schema breadth. It is also reasonable to use gSOAP to explore
less-common operations before deciding whether a smaller application-facing wrapper
is worthwhile.

### Compared with Privatehive/libONVIF

[Privatehive/libONVIF](https://github.com/Privatehive/libONVIF) is one of the closest
library-level alternatives to `wlonvif`. It wraps gSOAP in higher-level C++ and Qt
classes, uses RAII request/response wrappers, and states that most of the library is
thread-safe. Its documentation lists eleven client service domains: Analytics,
Device, Display, Events, Imaging, Media1, Media2, PTZ, Receiver, Recording, and
Replay. It also supplies dedicated Discovery and PullPoint helpers.

Key differences:

- libONVIF publishes a considerably broader ONVIF service surface. Applications
  needing Media2, Imaging, Analytics, Recording, Replay, Receiver, or Display should
  evaluate it before assuming those operations will be added to `wlonvif`.
- libONVIF hides most gSOAP details but still builds around gSOAP-generated bindings
  and Qt-oriented classes. `wlonvif` uses libcurl, libxml2, and OpenSSL directly and
  does not require Qt or generated SOAP sources.
- Qt/QML applications may benefit from libONVIF's design and its related ONVIFMonitor
  example. `wlonvif` is UI-framework-neutral and may require an application-specific
  adapter for Qt signals, objects, and event loops.
- Both provide WS-Discovery and PullPoint support. `wlonvif` additionally documents
  targeted unicast, bounded common-path HTTP probing, authorized CIDR scanning,
  evidence merging, host/concurrency limits, and progress reporting.
- `wlonvif` explicitly exposes deadlines, cancellation, endpoint authorization,
  parser/queue bounds, and a programmable virtual transport. libONVIF instead
  emphasizes service breadth, thread safety, RAII around gSOAP objects, and Qt
  integration.
- libONVIF is GPL-3.0. `wlonvif` is MIT. The application's distribution and linking
  requirements may make this difference decisive; obtain appropriate legal advice
  when license obligations are material.

Choose libONVIF when its much broader service list, existing Qt/QML integration, or
gSOAP-based operation model fits the application. Choose `wlonvif` when the required
workflow fits its narrower surface and avoiding Qt/gSOAP, using an MIT-licensed
dependency, explicit operation controls, robust direct-discovery fallbacks, or the
virtual test backend matters more. Neither documentation set proves support on a
particular device, so validate the exact methods and authentication behavior on the
target firmware.

### Compared with OnvifClient_gSOAP

[OnvifClient_gSOAP](https://github.com/Allforgot/OnvifClient_gSOAP) is a concrete
C++ ONVIF wrapper and sample project built on generated gSOAP code. Its published
sample demonstrates Device information and capabilities together with Media and PTZ
workflows, including AbsoluteMove and ContinuousMove. Both libraries expose these
operations; `wlonvif` also retains advertised operation-specific space URIs/ranges.

Key differences:

- OnvifClient_gSOAP exposes gSOAP-derived ONVIF bindings and expects gSOAP plus
  generated ONVIF client code as prerequisites. `wlonvif` uses its own curated
  service clients and does not require a SOAP code-generation step.
- Its README instructs users to configure camera values in a sample header.
  `wlonvif` examples use command-line, environment, protected-file, and interactive
  credential sources so device-specific values need not be compiled into examples.
- OnvifClient_gSOAP documents remote discovery as unfinished. `wlonvif` treats
  discovery as a primary feature and combines multicast, targeted unicast, bounded
  CIDR probing, and duplicate suppression.
- `wlonvif` has a curated PTZ surface: absolute, relative, and continuous movement,
  status, Stop, presets, preset navigation, and a stateful manual control policy.

Choose OnvifClient_gSOAP when an existing gSOAP toolchain is welcome, its binding
style matches the application, or its demonstrated AbsoluteMove workflow is closer
to the requirement. Choose `wlonvif` when self-contained CMake integration, robust
discovery, runtime credential handling, explicit operation bounds, and the virtual
test backend are higher priorities. Review both APIs against the exact required
operations; neither project's README establishes complete ONVIF coverage.

### Compared with onvif-qt-server-client

[onvif-qt-server-client](https://github.com/hummatli/onvif-qt-server-client) is a Qt
C++ and gSOAP sample containing ONVIF client code, server code, and generated service
libraries. Its published structure covers a wide range of domains, including Device,
Discovery, Events, Analytics, DeviceIO, Display, Imaging, Media, PTZ, Receiver,
Recording Control/Search, and Replay. The included test workflow runs its ONVIF
server and then uses its client to discover and call it.

Key differences:

- This project demonstrates both network client and network server roles. `wlonvif`
  provides a network client and an in-process virtual test transport, but not a
  general network-visible ONVIF server implementation.
- Its broad library structure follows Qt and generated gSOAP services. `wlonvif`
  offers fewer operations through smaller C++ value types and named service clients.
- Its documented development environment is Qt 5.5, Visual Studio 2013, and 32-bit
  Windows. That does not necessarily prevent porting, but consumers should budget
  for modernization and verify current compilers, dependencies, and target platforms.
- The project is Apache-2.0 licensed, while `wlonvif` is MIT-licensed. Both are
  permissive licenses, but their notice and attribution terms differ.
- `wlonvif` documents a standalone CMake package, cross-platform dependencies,
  bounded network/parser behavior, endpoint policy, cancellation/deadlines, and
  operational examples. onvif-qt-server-client provides something `wlonvif` does
  not: source material for implementing and testing the server side of many ONVIF
  domains.

Choose onvif-qt-server-client when studying or prototyping both ends of ONVIF, when
its generated server skeletons are needed, or when maintaining a compatible older Qt
stack. Choose `wlonvif` for a current C++20 client-only integration with explicit
runtime controls and no Qt/gSOAP dependency. For a new production server, treat the
Qt project as reference material first and independently assess the modernization,
security, conformance, and interoperability work required.

### Compared with ONVIF-HTTP-code

[ONVIF-HTTP-code](https://github.com/fernandorpardo/ONVIF-HTTP-code) deliberately
works close to HTTP and XML so readers can see the protocol rather than delegate it
to generated SOAP infrastructure. Its author describes it as partial and documents
behavior observed on several TP-Link Tapo models. It includes particularly useful
protocol explanations and Tapo-focused PTZ examples using RelativeMove and
AbsoluteMove.

Key differences:

- Both projects avoid making generated WSDL bindings their public interface, but
  ONVIF-HTTP-code emphasizes transparent, educational protocol code. `wlonvif`
  emphasizes a reusable library API with structured results and hidden SOAP details.
- ONVIF-HTTP-code documents real behavior on specific Tapo cameras and includes
  AbsoluteMove. That focus can be more useful than a generic abstraction when those
  devices are the deployment target.
- `wlonvif` supplies asynchronous tasks, cancellation/deadlines, bounded parsers and
  queues, endpoint policy, installable CMake targets, a virtual backend, and a larger
  suite of standalone operational examples.
- `wlonvif` provides multicast and direct-probe discovery as a library facility.
  ONVIF-HTTP-code is not presented by its author as a general robust discovery
  toolkit.
- ONVIF-HTTP-code discusses push-event experiments and notes limitations in Renew
  and Unsubscribe. `wlonvif` currently focuses on managed PullPoint event streams;
  it does not provide a general notification web server.

Choose ONVIF-HTTP-code to learn or debug the wire protocol, to build directly on its
documented Tapo experiments, or when its AbsoluteMove implementation matches the
need. Choose `wlonvif` for a more encapsulated application library, explicit
resource-control primitives, reusable discovery, and hardware-independent tests.
For difficult interoperability problems, reading ONVIF-HTTP-code alongside
`wlonvif` traces can be useful even if it is not the final dependency.

These projects can complement one another. A generated client or protocol-level
reference is useful for investigating an operation that has not yet been added to
`wlonvif`, while `wlonvif` can provide a smaller operational layer for the workflows
it supports.

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

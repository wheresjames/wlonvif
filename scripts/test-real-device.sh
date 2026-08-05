#!/usr/bin/env sh
set -eu

if [ -z "${WLONVIF_DEVICE_URL:-}" ] && [ -z "${WLONVIF_DEVICE_IP:-}" ]; then
  echo "Set WLONVIF_DEVICE_URL or WLONVIF_DEVICE_IP for an authorized ONVIF device" >&2
  exit 2
fi
if [ -n "${WLONVIF_DEVICE_URL:-}" ] && [ -n "${WLONVIF_DEVICE_IP:-}" ]; then
  echo "Set only one of WLONVIF_DEVICE_URL and WLONVIF_DEVICE_IP" >&2
  exit 2
fi

build_dir=${WLONVIF_BUILD_DIR:-build}
if [ -n "${WLONVIF_DEVICE_IP:-}" ]; then
  set -- --ip "$WLONVIF_DEVICE_IP" --timeout "${WLONVIF_TIMEOUT_SECONDS:-15}"
else
  set -- --url "$WLONVIF_DEVICE_URL" --timeout "${WLONVIF_TIMEOUT_SECONDS:-15}"
fi

if [ -n "${WLONVIF_USERNAME:-}" ]; then
  set -- "$@" --username "$WLONVIF_USERNAME"
fi

# The executable reads WLONVIF_PASSWORD itself. The password is never placed in
# process arguments or printed by this script.
"$build_dir/examples/device-info/wlonvif-device-info" "$@" --all
"$build_dir/examples/camera-health/wlonvif-camera-health" "$@"

if [ "${WLONVIF_TEST_MEDIA:-0}" = "1" ]; then
  "$build_dir/examples/media/wlonvif-media" "$@" profiles
fi

if [ "${WLONVIF_TEST_PTZ_STATUS:-0}" = "1" ]; then
  if [ -z "${WLONVIF_PROFILE_TOKEN:-}" ]; then
    echo "WLONVIF_PROFILE_TOKEN is required for PTZ status" >&2
    exit 2
  fi
  "$build_dir/examples/ptz/wlonvif-ptz" "$@" \
    --profile "$WLONVIF_PROFILE_TOKEN" status
fi

if [ "${WLONVIF_TEST_PTZ_MOVE:-0}" = "1" ]; then
  if [ -z "${WLONVIF_PROFILE_TOKEN:-}" ]; then
    echo "WLONVIF_PROFILE_TOKEN is required for PTZ movement" >&2
    exit 2
  fi
  echo "WARNING: performing explicitly enabled, small PTZ movement" >&2
  "$build_dir/examples/ptz/wlonvif-ptz" "$@" \
    --profile "$WLONVIF_PROFILE_TOKEN" move --pan 0.05 --confirm-movement
fi

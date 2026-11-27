#!/usr/bin/env bash
set -e

IMAGE_NAME="dnfui-dev"
CONTAINER_NAME="dnfui-tui"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/container_runtime.sh"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if ! container_image_exists "$IMAGE_NAME"; then
  container_missing_image_message
  exit 1
fi

"$CONTAINER_RUNTIME" run --rm -it \
  --name "$CONTAINER_NAME" \
  --init \
  -w /workspace \
  -e FINAL \
  -e ASAN \
  -e DEBUG_TRACE \
  -e TERM \
  -e DNFUI_MESON_BUILD_ROOT=/tmp/dnfui-build \
  -v "$PROJECT_ROOT:/workspace:ro" \
  "$IMAGE_NAME" \
  bash -c 'BUILD_DIR="$(./utils/meson_build.sh build-dir)" &&
    ./utils/meson_build.sh tui &&
    mkdir -p /run/dbus &&
    dbus-daemon --system --fork &&
    exec "$BUILD_DIR/src/dnfui-tui"'

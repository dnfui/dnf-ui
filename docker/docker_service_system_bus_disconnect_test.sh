#!/usr/bin/env bash
set -e

# Docker wrapper for the system bus orphan cleanup smoke test.

# Print colors:
FMT_RED=$(printf '\033[31m')
FMT_GREEN=$(printf '\033[32m')
FMT_RESET=$(printf '\033[0m')

color_print() {
  local color="$1"
  shift
  echo -e "${color}$*${FMT_RESET}"
}

IMAGE_NAME="dnfui-dev"
CONTAINER_NAME="dnfui-service-system-bus-disconnect-test"

# Make this script work from any directory:
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/container_runtime.sh"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HOST_DIR="$PROJECT_ROOT"

# Ensure image exists:
if ! container_image_exists "$IMAGE_NAME"; then
  color_print "$FMT_RED" "$(container_missing_image_message)"
  exit 1
fi

color_print "$FMT_GREEN" "*** Running transaction service system bus disconnect test inside container... ***"

"$CONTAINER_RUNTIME" run --rm \
  --name "$CONTAINER_NAME" \
  --init \
  -w /workspace \
  -e FINAL \
  -e ASAN \
  -e DEBUG_TRACE \
  -e DNFUI_MESON_BUILD_ROOT=/tmp/dnfui-build \
  -e SERVICE_TEST_INSTALL_SPEC=cowsay \
  -e SERVICE_SYSTEM_BUS_DISCONNECT=yes \
  -v "$HOST_DIR:/workspace" \
  "$IMAGE_NAME" \
  bash -c 'BUILD_DIR="$(./utils/meson_build.sh build-dir)" && ./utils/meson_build.sh service-tests && SERVICE_TEST_DISABLE_PREVIEW_AUTHORIZATION=1 SERVICE_BIN="$BUILD_DIR/src/service/dnfui-service-tests" bash /workspace/docker/docker_service_system_bus_inner.sh'

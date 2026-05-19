#!/usr/bin/env bash
set -e

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
CONTAINER_NAME="dnfui-test"
DOCKER_TTY_ARGS=()
if [ "${DOCKER_USE_TTY:-1}" != "0" ]; then
  DOCKER_TTY_ARGS=(-it)
fi

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

color_print "$FMT_GREEN" "*** Running test suite inside container... ***"

"$CONTAINER_RUNTIME" run --rm "${DOCKER_TTY_ARGS[@]}" \
  --name "$CONTAINER_NAME" \
  --init \
  -w /workspace \
  -e FINAL \
  -e ASAN \
  -e DNFUI_MESON_BUILD_ROOT=/tmp/dnfui-build \
  -v "$HOST_DIR:/workspace" \
  "$IMAGE_NAME" \
  bash -c 'BUILD_DIR="$(./utils/meson_build.sh build-dir)" && ./utils/meson_build.sh service-tests tests && meson test -C "$BUILD_DIR" --print-errorlogs'

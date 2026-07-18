#!/usr/bin/env bash
set -e

# Docker wrapper for dnf5daemon transaction client tests.
# It runs the tagged tests with a system bus so the client uses the real daemon.

FMT_RED=$(printf '\033[31m')
FMT_GREEN=$(printf '\033[32m')
FMT_RESET=$(printf '\033[0m')

color_print() {
  local color="$1"
  shift
  echo -e "${color}$*${FMT_RESET}"
}

IMAGE_NAME="dnfui-dev"
CONTAINER_NAME="dnfui-dnf5daemon-test"
CACHE_VOLUME_NAME="${CACHE_VOLUME_NAME:-dnfui-dnf5daemon-test-cache}"
INSTALL_SPEC="${DNFUI_TEST_DNF5DAEMON_INSTALL_SPEC:-cowsay}"
INSTALL_NAME="${DNFUI_TEST_DNF5DAEMON_INSTALL_NAME:-$INSTALL_SPEC}"
DOWNGRADE_SPEC="${DNFUI_TEST_DNF5DAEMON_DOWNGRADE_SPEC:-}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/container_runtime.sh"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HOST_DIR="$PROJECT_ROOT"

if ! container_image_exists "$IMAGE_NAME"; then
  color_print "$FMT_RED" "$(container_missing_image_message)"
  exit 1
fi

cleanup() {
  "$CONTAINER_RUNTIME" volume rm -f "$CACHE_VOLUME_NAME" >/dev/null 2>&1 || true
}
trap cleanup EXIT

"$CONTAINER_RUNTIME" volume create "$CACHE_VOLUME_NAME" >/dev/null

color_print "$FMT_GREEN" "*** Running dnf5daemon transaction client tests... ***"
"$CONTAINER_RUNTIME" run --rm \
  --name "$CONTAINER_NAME" \
  --init \
  -w /workspace \
  -e FINAL \
  -e ASAN \
  -e DEBUG_TRACE \
  -e DNFUI_MESON_BUILD_ROOT=/tmp/dnfui-build \
  -e DNFUI_TEST_DNF5DAEMON=1 \
  -e DNFUI_TEST_DNF5DAEMON_INSTALL_SPEC="$INSTALL_SPEC" \
  -e DNFUI_TEST_DNF5DAEMON_INSTALL_NAME="$INSTALL_NAME" \
  -e DNFUI_TEST_DNF5DAEMON_DOWNGRADE_SPEC="$DOWNGRADE_SPEC" \
  -v "$HOST_DIR:/workspace" \
  -v "$CACHE_VOLUME_NAME:/var/cache/libdnf5" \
  "$IMAGE_NAME" \
  bash /workspace/docker/docker_dnf5daemon_test_inner.sh
